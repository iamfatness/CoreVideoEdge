#include "edge/IsoMp4Writer.h"

#include "core/BoundedAsyncLog.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

}  // namespace

IsoMp4Writer::~IsoMp4Writer() {
  if (writing_) {
    std::string ignored;
    (void)finalize(0, ignored);
  }
  reset();
}

void IsoMp4Writer::reset() {
  sinkWriter_.Reset();
  if (mediaSink_) (void)mediaSink_->Shutdown();
  mediaSink_.Reset();
  if (byteStream_) (void)byteStream_->Close();
  byteStream_.Reset();
  pendingBuffer_.Reset();
  writing_ = false;
}

bool IsoMp4Writer::open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error) {
  reset();
  path_ = path;
  settings_ = settings;
  settings_.width = std::max(2, settings.width & ~1);
  settings_.height = std::max(2, settings.height & ~1);
  settings_.nominalFps = std::max(1, settings.nominalFps);
  frameDuration100ns_ = 10'000'000LL / (settings.constantFps == 30 || settings.constantFps == 60 ? settings.constantFps
                                                                                              : settings_.nominalFps);
  pendingPts100ns_ = -1;
  lastVideoPts100ns_ = -1;
  videoEnd100ns_ = 0;
  videoFrames_ = prerollFrames_ = audioSamples_ = 0;
  heldRepeats_ = videoDropped_ = backlogPeak_ = cadenceRepeats_ = cadenceSkipped_ = 0;
  pendingIndex_ = -1;
  settings_.constantFps = settings.constantFps == 30 || settings.constantFps == 60 ? settings.constantFps : 0;
  pendingIsRepeat_ = false;
  encoderPath_.clear();

  // ---- media types ---------------------------------------------------------
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

  // ---- fragmented MP4 sink (Pro's createFragmentedSink) --------------------
  HRESULT result = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE,
                                path_.wstring().c_str(), &byteStream_);
  if (FAILED(result)) {
    error = "could not create " + path_.string() + " (" + hr(result) + ")";
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
  videoStream_ = 0;
  audioStream_ = 1;
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
  writing_ = true;
  detectEncoderPath();

  // Pin the video track to the epoch with one black frame at t=0. Its duration
  // becomes the gap to the first real frame.
  const DWORD bytes = static_cast<DWORD>(settings_.width) * settings_.height * 3 / 2;
  ComPtr<IMFMediaBuffer> black;
  BYTE* pixels = nullptr;
  if (FAILED(MFCreateMemoryBuffer(bytes, &black)) || FAILED(black->Lock(&pixels, nullptr, nullptr))) {
    error = "could not allocate the preroll frame";
    reset();
    return false;
  }
  const size_t luma = static_cast<size_t>(settings_.width) * settings_.height;
  std::memset(pixels, 0, luma);               // full-range black
  std::memset(pixels + luma, 128, luma / 2);  // neutral chroma
  black->Unlock();
  black->SetCurrentLength(bytes);
  if (!queueVideoBuffer(black.Get(), 0, true, error)) {
    reset();
    return false;
  }
  return true;
}

void IsoMp4Writer::detectEncoderPath() {
  // Report the encoder Media Foundation actually instantiated, the way Pro does
  // (#935): a hardware MFT carries a device URL / vendor id attribute; the OS
  // software H.264 MFT carries neither. A hardware REQUEST served by software
  // reads "software".
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

bool IsoMp4Writer::writeVideoSample(IMFSample* sample, std::string& error) {
  const HRESULT result = sinkWriter_->WriteSample(videoStream_, sample);
  if (FAILED(result)) {
    error = "video WriteSample failed (" + hr(result) + ")";
    return false;
  }
  return true;
}

// Holds `buffer` until the next one arrives, so every sample's duration is the
// real gap to its successor (VFR). The previously held sample is written now.
bool IsoMp4Writer::queueVideoBuffer(IMFMediaBuffer* buffer, std::int64_t pts100ns, bool preroll, std::string& error) {
  if (pendingBuffer_ && !flushPendingVideo(pts100ns, error)) return false;
  pendingBuffer_ = buffer;
  pendingPts100ns_ = pts100ns;
  pendingIsPreroll_ = preroll;
  pendingIsRepeat_ = false;
  pendingIndex_ = constantRate() ? gridIndex(pts100ns) : -1;
  lastVideoPts100ns_ = pts100ns;
  return true;
}

std::int64_t IsoMp4Writer::gridIndex(std::int64_t pts100ns) const {
  const std::int64_t fps = settings_.constantFps;
  return (pts100ns * fps + 5'000'000) / 10'000'000;  // nearest slot
}

std::int64_t IsoMp4Writer::gridPts(std::int64_t index) const {
  return index * 10'000'000 / settings_.constantFps;
}

bool IsoMp4Writer::fillGridTo(std::int64_t index, std::string& error, bool stall) {
  while (pendingBuffer_ && pendingIndex_ + 1 < index) {
    const Microsoft::WRL::ComPtr<IMFMediaBuffer> held = pendingBuffer_;
    const auto next = pendingIndex_ + 1;
    if (!flushPendingVideo(gridPts(next), error)) return false;
    pendingBuffer_ = held;
    pendingPts100ns_ = gridPts(next);
    pendingIsPreroll_ = false;
    pendingIsRepeat_ = true;
    pendingIsCadence_ = !stall;
    pendingIndex_ = next;
    lastVideoPts100ns_ = pendingPts100ns_;
  }
  return true;
}

std::int64_t IsoMp4Writer::videoBacklog() const {
  if (!sinkWriter_) return 0;
  MF_SINK_WRITER_STATISTICS stats{};
  stats.cb = sizeof(stats);
  if (FAILED(sinkWriter_->GetStatistics(videoStream_, &stats))) return 0;
  return stats.qwNumSamplesReceived > stats.qwNumSamplesProcessed
             ? static_cast<std::int64_t>(stats.qwNumSamplesReceived - stats.qwNumSamplesProcessed)
             : 0;
}

bool IsoMp4Writer::repeatHeldVideo(std::int64_t pts100ns, std::string& error) {
  if (!writing_ || !pendingBuffer_ || pts100ns <= pendingPts100ns_) return true;
  if (videoBacklog() > settings_.maxVideoBacklog) return true;  // the held frame just runs longer
  if (constantRate()) return fillGridTo(gridIndex(pts100ns) + 1, error, true);  // the grid IS the repeat
  const Microsoft::WRL::ComPtr<IMFMediaBuffer> held = pendingBuffer_;
  if (!flushPendingVideo(pts100ns, error)) return false;
  pendingBuffer_ = held;
  pendingPts100ns_ = pts100ns;
  pendingIsPreroll_ = false;
  pendingIsRepeat_ = true;
  pendingIsCadence_ = false;
  lastVideoPts100ns_ = pts100ns;
  return true;
}

bool IsoMp4Writer::flushPendingVideo(std::int64_t end100ns, std::string& error) {
  if (!pendingBuffer_) return true;
  ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample))) {
    error = "could not create a video sample";
    return false;
  }
  sample->AddBuffer(pendingBuffer_.Get());
  sample->SetSampleTime(pendingPts100ns_);
  const std::int64_t duration = std::max<std::int64_t>(1, end100ns - pendingPts100ns_);
  sample->SetSampleDuration(duration);
  if (!writeVideoSample(sample.Get(), error)) return false;
  videoEnd100ns_ = pendingPts100ns_ + duration;
  if (pendingIsPreroll_) ++prerollFrames_;
  else if (pendingIsRepeat_ && pendingIsCadence_) ++cadenceRepeats_;
  else if (pendingIsRepeat_) ++heldRepeats_;
  else ++videoFrames_;
  pendingBuffer_.Reset();
  return true;
}

bool IsoMp4Writer::writeVideo(const std::uint8_t* nv12, std::int64_t pts100ns, std::string& error) {
  if (!writing_ || !nv12) {
    error = "writer is not open";
    return false;
  }
  std::int64_t index = -1;
  if (constantRate()) {
    index = gridIndex(pts100ns);
    pts100ns = gridPts(index);
  } else if (pts100ns <= lastVideoPts100ns_) {
    pts100ns = lastVideoPts100ns_ + 1;  // MF needs strictly increasing
  }
  // Hard bound on memory: never queue past the backlog cap. A dropped frame
  // is counted and the held frame simply runs longer (VFR).
  const auto backlog = videoBacklog();
  backlogPeak_ = std::max(backlogPeak_, backlog);
  if (backlog > settings_.maxVideoBacklog) {
    ++videoDropped_;
    return true;
  }
  const DWORD bytes = static_cast<DWORD>(settings_.width) * settings_.height * 3 / 2;
  ComPtr<IMFMediaBuffer> buffer;
  BYTE* locked = nullptr;
  HRESULT result = MFCreateMemoryBuffer(bytes, &buffer);
  if (FAILED(result) || FAILED(result = buffer->Lock(&locked, nullptr, nullptr))) {
    error = "could not allocate a video buffer (" + hr(result) + ")";
    return false;
  }
  std::memcpy(locked, nv12, bytes);
  buffer->Unlock();
  buffer->SetCurrentLength(bytes);
  if (constantRate()) {
    if (pendingBuffer_ && index <= pendingIndex_) {
      // Same grid slot (or a late arrival): the newest picture wins the slot.
      if (!pendingIsPreroll_ && !pendingIsRepeat_) ++cadenceSkipped_;
      pendingBuffer_ = buffer;
      pendingIsPreroll_ = false;
      pendingIsRepeat_ = false;
      return true;
    }
    if (!fillGridTo(index, error)) return false;
  }
  return queueVideoBuffer(buffer.Get(), pts100ns, false, error);
}

bool IsoMp4Writer::writePcm16(const std::int16_t* samples, std::int64_t frames, std::string& error) {
  if (frames <= 0) return true;
  const DWORD bytes = static_cast<DWORD>(frames * 4);
  ComPtr<IMFMediaBuffer> buffer;
  BYTE* locked = nullptr;
  HRESULT result = MFCreateMemoryBuffer(bytes, &buffer);
  if (FAILED(result) || FAILED(result = buffer->Lock(&locked, nullptr, nullptr))) {
    error = "could not allocate an audio buffer (" + hr(result) + ")";
    return false;
  }
  if (samples) std::memcpy(locked, samples, bytes);
  else std::memset(locked, 0, bytes);
  buffer->Unlock();
  buffer->SetCurrentLength(bytes);
  ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample))) {
    error = "could not create an audio sample";
    return false;
  }
  sample->AddBuffer(buffer.Get());
  // Sample-counted: endpoints rounded once from the running total, never per
  // block, so the track has no cumulative rounding drift (Pro's rule).
  const std::int64_t rate = settings_.audioSampleRate;
  const std::int64_t start = audioSamples_ * 10'000'000LL / rate;
  const std::int64_t end = (audioSamples_ + frames) * 10'000'000LL / rate;
  sample->SetSampleTime(start);
  sample->SetSampleDuration(end - start);
  result = sinkWriter_->WriteSample(audioStream_, sample.Get());
  if (FAILED(result)) {
    error = "audio WriteSample failed (" + hr(result) + ")";
    return false;
  }
  audioSamples_ += frames;
  return true;
}

bool IsoMp4Writer::writeAudio(const float* stereo, std::int64_t frames, std::string& error) {
  if (!writing_) {
    error = "writer is not open";
    return false;
  }
  if (!stereo || frames <= 0) return true;
  pcm16_.resize(static_cast<size_t>(frames) * 2);
  for (size_t i = 0; i < pcm16_.size(); ++i) {
    const float clamped = std::clamp(stereo[i], -1.0f, 1.0f);
    pcm16_[i] = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
  }
  return writePcm16(pcm16_.data(), frames, error);
}

bool IsoMp4Writer::writeSilence(std::int64_t frames, std::string& error) {
  if (!writing_) {
    error = "writer is not open";
    return false;
  }
  // Chunked (0.1 s) so a guest who first speaks minutes in never becomes one
  // giant sample (Pro's writeAudioSilence).
  const std::int64_t chunk = settings_.audioSampleRate / 10;
  while (frames > 0) {
    const auto n = std::min(frames, chunk);
    if (!writePcm16(nullptr, n, error)) return false;
    frames -= n;
  }
  return true;
}

std::int64_t IsoMp4Writer::videoEnd100ns() const {
  return pendingBuffer_ ? std::max(videoEnd100ns_, pendingPts100ns_ + frameDuration100ns_) : videoEnd100ns_;
}

std::int64_t IsoMp4Writer::audioEnd100ns() const {
  return audioSamples_ * 10'000'000LL / settings_.audioSampleRate;
}

std::int64_t IsoMp4Writer::bytesOnDisk() const {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path_, ec);
  return ec ? 0 : static_cast<std::int64_t>(size);
}

bool IsoMp4Writer::finalize(std::int64_t minimumEnd100ns, std::string& error) {
  if (!writing_) {
    error = "writer was not open";
    return false;
  }
  bool ok = true;
  std::string firstError;
  // One common end for both tracks: the later of the two, and at least the
  // take's stop time. Video: the held last frame extends to it. Audio: silence.
  const std::int64_t end = std::max({minimumEnd100ns, videoEnd100ns(), audioEnd100ns()});
  if (constantRate() && !fillGridTo(gridIndex(end), firstError)) ok = false;
  if (ok && !flushPendingVideo(end, firstError)) ok = false;
  const std::int64_t rate = settings_.audioSampleRate;
  const std::int64_t endFrame = (end * rate + 9'999'999) / 10'000'000;
  if (ok && endFrame > audioSamples_ && !writeSilence(endFrame - audioSamples_, firstError)) ok = false;

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
  if (FAILED(result) && ok) {
    ok = false;
    firstError = "Finalize failed (" + hr(result) + ")";
  }
  reset();
  if (ok && bytesOnDisk() <= 0) {
    ok = false;
    firstError = "the finalized file is empty";
  }
  core::nativeLogf("[iso] finalized %s frames=%lld preroll=%lld held=%lld dropped=%lld backlogPeak=%lld "
                   "audioSamples=%lld end=%.3fs ok=%d %s\n",
                   path_.filename().string().c_str(), static_cast<long long>(videoFrames_),
                   static_cast<long long>(prerollFrames_), static_cast<long long>(heldRepeats_),
                   static_cast<long long>(videoDropped_), static_cast<long long>(backlogPeak_),
                   static_cast<long long>(audioSamples_),
                   end / 10'000'000.0, ok ? 1 : 0, firstError.c_str());
  if (!ok) error = firstError;
  return ok;
}

}  // namespace corevideo::edge
