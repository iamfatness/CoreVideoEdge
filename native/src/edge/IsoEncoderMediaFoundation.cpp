// Windows ISO backend: Media Foundation Sink Writer into a fragmented MP4 sink.
// Moved verbatim out of IsoMp4Writer.cpp when the writer's timing rules were
// made portable (macOS port, 2026-11-09); behaviour is unchanged.

#include "edge/IsoEncoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace corevideo::edge {
namespace {

std::string hr(HRESULT result) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
  return text;
}

// Zoom raw I420 is requested as full-range BT.709 (engine join param
// VideoRawdataColorspace_BT709_F). Say so in the stream so players and NLEs do
// not guess (same as Pro's setBt709FullRange).
void setBt709FullRange(IMFMediaType* type) {
  type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
}

class MfFrame final : public IsoEncoderBackend::Frame {
 public:
  explicit MfFrame(ComPtr<IMFMediaBuffer> b) : buffer(std::move(b)) {}
  ComPtr<IMFMediaBuffer> buffer;
};

class MediaFoundationIsoEncoder final : public IsoEncoderBackend {
 public:
  ~MediaFoundationIsoEncoder() override { reset(); }

  bool open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error) override {
    reset();
    settings_ = settings;
    ComPtr<IMFMediaType> videoOut, videoIn, audioOut, audioIn;
    if (FAILED(MFCreateMediaType(&videoOut)) || FAILED(MFCreateMediaType(&videoIn)) ||
        FAILED(MFCreateMediaType(&audioOut)) || FAILED(MFCreateMediaType(&audioIn))) {
      error = "could not create Media Foundation media types";
      return false;
    }
    videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    videoOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    videoOut->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::max(1, settings_.videoBitrateMbps) * 1'000'000));
    videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(videoOut.Get(), MF_MT_FRAME_SIZE, settings_.width, settings_.height);
    MFSetAttributeRatio(videoOut.Get(), MF_MT_FRAME_RATE, settings_.nominalFps, 1);
    MFSetAttributeRatio(videoOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    setBt709FullRange(videoOut.Get());

    videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    videoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    videoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(settings_.width));
    MFSetAttributeSize(videoIn.Get(), MF_MT_FRAME_SIZE, settings_.width, settings_.height);
    MFSetAttributeRatio(videoIn.Get(), MF_MT_FRAME_RATE, settings_.nominalFps, 1);
    MFSetAttributeRatio(videoIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    setBt709FullRange(videoIn.Get());

    const UINT32 rate = static_cast<UINT32>(settings_.audioSampleRate);
    audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                        static_cast<UINT32>(std::clamp(settings_.audioBitrateKbps, 96, 320) * 1000 / 8));
    audioOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

    audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);

    // ---- fragmented MP4 sink (Pro's createFragmentedSink) ------------------
    HRESULT result = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE,
                                  path.wstring().c_str(), &byteStream_);
    if (FAILED(result)) {
      error = "could not create " + path.string() + " (" + hr(result) + ")";
      return false;
    }
    result = MFCreateFMPEG4MediaSink(byteStream_.Get(), videoOut.Get(), audioOut.Get(), &mediaSink_);
    if (FAILED(result)) {
      error = "could not create the MP4 sink (" + hr(result) + ")";
      reset();
      return false;
    }
    ComPtr<IMFAttributes> sinkAttributes;
    if (SUCCEEDED(mediaSink_.As(&sinkAttributes))) {
      (void)sinkAttributes->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, 10'000'000ULL);
    }
    ComPtr<IMFAttributes> writerAttributes;
    MFCreateAttributes(&writerAttributes, 2);
    writerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, settings_.hardwareEncode ? TRUE : FALSE);
    result = writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (FAILED(result)) {
      error = "could not disable Sink Writer throttling (" + hr(result) + ")";
      reset();
      return false;
    }
    result = MFCreateSinkWriterFromMediaSink(mediaSink_.Get(), writerAttributes.Get(), &sinkWriter_);
    if (FAILED(result)) {
      error = "could not create the Sink Writer (" + hr(result) + ")";
      reset();
      return false;
    }
    result = sinkWriter_->SetInputMediaType(videoStream_, videoIn.Get(), nullptr);
    if (FAILED(result)) {
      error = std::string("no ") + (settings_.hardwareEncode ? "hardware" : "software") +
              " H.264 encoder accepted 1080p NV12 (" + hr(result) + ")";
      reset();
      return false;
    }
    result = sinkWriter_->SetInputMediaType(audioStream_, audioIn.Get(), nullptr);
    if (FAILED(result)) {
      error = "the AAC encoder refused 48 kHz stereo PCM (" + hr(result) + ")";
      reset();
      return false;
    }
    result = sinkWriter_->BeginWriting();
    if (FAILED(result)) {
      error = "could not begin writing (" + hr(result) + ")";
      reset();
      return false;
    }
    detectEncoderPath();
    return true;
  }

  FramePtr makeFrame(const std::uint8_t* nv12, std::string& error) override {
    const DWORD bytes = static_cast<DWORD>(settings_.width) * settings_.height * 3 / 2;
    ComPtr<IMFMediaBuffer> buffer;
    BYTE* locked = nullptr;
    HRESULT result = MFCreateMemoryBuffer(bytes, &buffer);
    if (FAILED(result) || FAILED(result = buffer->Lock(&locked, nullptr, nullptr))) {
      error = "could not allocate a video buffer (" + hr(result) + ")";
      return nullptr;
    }
    if (nv12) {
      std::memcpy(locked, nv12, bytes);
    } else {
      const size_t luma = static_cast<size_t>(settings_.width) * settings_.height;
      std::memset(locked, 0, luma);               // full-range black
      std::memset(locked + luma, 128, luma / 2);  // neutral chroma
    }
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    return std::make_shared<MfFrame>(std::move(buffer));
  }

  bool writeVideo(const FramePtr& frame, std::int64_t pts100ns, std::int64_t duration100ns,
                  std::string& error) override {
    auto* mf = static_cast<MfFrame*>(frame.get());
    ComPtr<IMFSample> sample;
    if (!mf || FAILED(MFCreateSample(&sample))) {
      error = "could not create a video sample";
      return false;
    }
    sample->AddBuffer(mf->buffer.Get());
    sample->SetSampleTime(pts100ns);
    sample->SetSampleDuration(duration100ns);
    const HRESULT result = sinkWriter_->WriteSample(videoStream_, sample.Get());
    if (FAILED(result)) {
      error = "video WriteSample failed (" + hr(result) + ")";
      return false;
    }
    return true;
  }

  bool writeAudio(const std::int16_t* pcm, std::int64_t startSample, std::int64_t frames,
                  std::string& error) override {
    const DWORD bytes = static_cast<DWORD>(frames * 4);
    ComPtr<IMFMediaBuffer> buffer;
    BYTE* locked = nullptr;
    HRESULT result = MFCreateMemoryBuffer(bytes, &buffer);
    if (FAILED(result) || FAILED(result = buffer->Lock(&locked, nullptr, nullptr))) {
      error = "could not allocate an audio buffer (" + hr(result) + ")";
      return false;
    }
    if (pcm) std::memcpy(locked, pcm, bytes);
    else std::memset(locked, 0, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) {
      error = "could not create an audio sample";
      return false;
    }
    sample->AddBuffer(buffer.Get());
    // Endpoints rounded once from the running total, never per block.
    const std::int64_t rate = settings_.audioSampleRate;
    const std::int64_t start = startSample * 10'000'000LL / rate;
    const std::int64_t end = (startSample + frames) * 10'000'000LL / rate;
    sample->SetSampleTime(start);
    sample->SetSampleDuration(end - start);
    result = sinkWriter_->WriteSample(audioStream_, sample.Get());
    if (FAILED(result)) {
      error = "audio WriteSample failed (" + hr(result) + ")";
      return false;
    }
    return true;
  }

  std::int64_t videoBacklog() const override {
    if (!sinkWriter_) return 0;
    MF_SINK_WRITER_STATISTICS stats{};
    stats.cb = sizeof(stats);
    if (FAILED(sinkWriter_->GetStatistics(videoStream_, &stats))) return 0;
    return stats.qwNumSamplesReceived > stats.qwNumSamplesProcessed
               ? static_cast<std::int64_t>(stats.qwNumSamplesReceived - stats.qwNumSamplesProcessed)
               : 0;
  }

  bool finalize(std::int64_t, std::string& error) override {
    if (!sinkWriter_) {
      error = "writer was not open";
      return false;
    }
    // Pro: signal EOS and give the encoder a bounded chance to drain before the
    // fragmented sink writes its index. Never an unbounded wait.
    if (SUCCEEDED(sinkWriter_->NotifyEndOfSegment(MF_SINK_WRITER_ALL_STREAMS))) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      do {
        MF_SINK_WRITER_STATISTICS stats{};
        stats.cb = sizeof(stats);
        if (FAILED(sinkWriter_->GetStatistics(videoStream_, &stats)) ||
            stats.qwNumSamplesProcessed >= stats.qwNumSamplesReceived) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } while (std::chrono::steady_clock::now() < deadline);
    }
    const HRESULT result = sinkWriter_->Finalize();
    reset();
    if (FAILED(result)) {
      error = "Finalize failed (" + hr(result) + ")";
      return false;
    }
    return true;
  }

  void reset() override {
    sinkWriter_.Reset();
    if (mediaSink_) (void)mediaSink_->Shutdown();
    mediaSink_.Reset();
    if (byteStream_) (void)byteStream_->Close();
    byteStream_.Reset();
  }

  std::string encoderPath() const override { return encoderPath_; }

 private:
  void detectEncoderPath() {
    // Report the encoder Media Foundation actually instantiated, the way Pro
    // does (#935): a hardware MFT carries a device URL / vendor id attribute;
    // the OS software H.264 MFT carries neither. A hardware REQUEST served by
    // software reads "software".
    ComPtr<IMFSinkWriterEx> extended;
    if (FAILED(sinkWriter_.As(&extended))) return;
    for (DWORD index = 0; index < 8; ++index) {
      GUID category{};
      ComPtr<IMFTransform> transform;
      if (FAILED(extended->GetTransformForStream(videoStream_, index, &category, &transform))) break;
      if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
      ComPtr<IMFAttributes> attributes;
      bool hardware = false;
      if (SUCCEEDED(transform->GetAttributes(&attributes))) {
        UINT32 length = 0;
        hardware = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)) ||
                   SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &length));
      }
      encoderPath_ = hardware ? "hardware" : "software";
      return;
    }
  }

  IsoWriterSettings settings_;
  ComPtr<IMFByteStream> byteStream_;
  ComPtr<IMFMediaSink> mediaSink_;
  ComPtr<IMFSinkWriter> sinkWriter_;
  DWORD videoStream_ = 0;
  DWORD audioStream_ = 1;
  std::string encoderPath_;
};

}  // namespace

std::unique_ptr<IsoEncoderBackend> makeIsoEncoderBackend() { return std::make_unique<MediaFoundationIsoEncoder>(); }

IsoMediaThreadScope::IsoMediaThreadScope() {
  com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  mf_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  ok_ = com_ && mf_;
  if (!ok_) error_ = "Media Foundation is not available on this machine";
}

IsoMediaThreadScope::~IsoMediaThreadScope() {
  if (mf_) MFShutdown();
  if (com_) CoUninitialize();
}

}  // namespace corevideo::edge
