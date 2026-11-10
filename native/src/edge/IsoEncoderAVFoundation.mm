// macOS ISO backend: VideoToolbox H.264 -> AVAssetWriter (passthrough video,
// AAC encoded by AVAssetWriter from LPCM), fragmented MP4.
//
// Why VideoToolbox directly instead of letting AVAssetWriter encode:
//  - backlog: IsoMp4Writer's hard cap needs a real count of video samples that
//    have not reached the file (Media Foundation reports it from the Sink
//    Writer statistics). Here it is exact: frames handed to VT minus
//    compressed samples appended to the writer, including samples the writer
//    is holding back because the audio track has not caught up (the muxer
//    interleave hold that took down the first live Windows take).
//  - the measured encoder path ("hardware" | "software") comes from
//    kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder.
//  - colour: BT.709 primaries/transfer/matrix are set on the session and the
//    input buffers are full-range NV12, the same declaration the Windows
//    writer makes.
//
// Passthrough MP4 inputs need the H.264 format description before writing
// starts, so open() encodes the t=0 black preroll frame synchronously and uses
// its format description; that compressed sample is then the preroll sample
// IsoMp4Writer writes first (no second t=0 encode).
//
// Threading: all calls from the recorder thread. VT's output callback only
// queues compressed samples under a mutex; the AVAssetWriter is touched from
// the recorder thread alone.

#include "edge/IsoEncoder.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace corevideo::edge {
namespace {

constexpr int32_t kVideoTimescale = 90000;

std::string osStatus(const char* what, OSStatus status) {
  return std::string(what) + " (OSStatus " + std::to_string(static_cast<long>(status)) + ")";
}

std::string nsErrorText(NSError* error) {
  if (!error) return "unknown error";
  std::string text = error.localizedDescription ? error.localizedDescription.UTF8String : "error";
  if (NSError* under = error.userInfo[NSUnderlyingErrorKey]) {
    text += " (" + std::string(under.domain.UTF8String) + " " + std::to_string(static_cast<long>(under.code)) + ")";
  } else {
    text += " (" + std::string(error.domain.UTF8String) + " " + std::to_string(static_cast<long>(error.code)) + ")";
  }
  return text;
}

// 100 ns ticks -> 90 kHz ticks, rounded (1/60 s = 166666 ticks -> 1500).
int64_t toVideoTicks(int64_t t100ns) { return (t100ns * 9 + 500) / 1000; }

class AvFrame final : public IsoEncoderBackend::Frame {
 public:
  explicit AvFrame(CVPixelBufferRef p) : pixels(p) {}
  ~AvFrame() override {
    if (preencoded) CFRelease(preencoded);
    if (pixels) CVPixelBufferRelease(pixels);
  }
  CVPixelBufferRef pixels = nullptr;
  CMSampleBufferRef preencoded = nullptr;  // the t=0 probe encode, consumed by the first write
};

class AvIsoEncoder final : public IsoEncoderBackend {
 public:
  ~AvIsoEncoder() override { reset(); }

  bool open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error) override {
    reset();
    settings_ = settings;
    @autoreleasepool {
      if (!createSession(error)) {
        reset();
        return false;
      }
      // Probe encode: the black preroll at t=0, synchronously.
      CVPixelBufferRef black = createPixelBuffer(nullptr, error);
      if (!black) {
        reset();
        return false;
      }
      OSStatus status = VTCompressionSessionEncodeFrame(session_, black, CMTimeMake(0, kVideoTimescale),
                                                        kCMTimeInvalid, nullptr, nullptr, nullptr);
      if (status == noErr) status = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
      if (status != noErr) {
        CVPixelBufferRelease(black);
        error = osStatus("the H.264 encoder refused the first frame", status);
        reset();
        return false;
      }
      CMSampleBufferRef probe = nullptr;
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!encoded_.empty()) {
          probe = encoded_.front();
          encoded_.pop_front();
        }
      }
      if (!probe) {
        CVPixelBufferRelease(black);
        error = asyncError_.empty() ? "the H.264 encoder produced no first frame" : asyncError_;
        reset();
        return false;
      }
      CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(probe);

      // ---- the file -------------------------------------------------------
      NSString* file = [NSString stringWithUTF8String:path.string().c_str()];
      NSURL* url = [NSURL fileURLWithPath:file];
      [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
      NSError* nsError = nil;
      writer_ = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&nsError];
      if (!writer_) {
        CFRelease(probe);
        CVPixelBufferRelease(black);
        error = "could not create " + path.string() + ": " + nsErrorText(nsError);
        reset();
        return false;
      }
      writer_.shouldOptimizeForNetworkUse = NO;
      // Fragmented: a crash or power loss loses only the in-flight tail.
      writer_.movieFragmentInterval = CMTimeMake(1, 1);

      videoInput_ = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                   outputSettings:nil
                                                 sourceFormatHint:format];
      videoInput_.expectsMediaDataInRealTime = YES;
      videoInput_.mediaTimeScale = kVideoTimescale;

      AudioStreamBasicDescription pcm{};
      pcm.mSampleRate = settings_.audioSampleRate;
      pcm.mFormatID = kAudioFormatLinearPCM;
      pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      pcm.mBytesPerPacket = 4;
      pcm.mFramesPerPacket = 1;
      pcm.mBytesPerFrame = 4;
      pcm.mChannelsPerFrame = 2;
      pcm.mBitsPerChannel = 16;
      AudioChannelLayout layout{};
      layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
      status = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &pcm, sizeof(layout), &layout, 0, nullptr, nullptr,
                                              &pcmFormat_);
      if (status != noErr) {
        CFRelease(probe);
        CVPixelBufferRelease(black);
        error = osStatus("could not describe 48 kHz stereo PCM", status);
        reset();
        return false;
      }
      NSDictionary* aac = @{
        AVFormatIDKey : @(kAudioFormatMPEG4AAC),
        AVSampleRateKey : @(settings_.audioSampleRate),
        AVNumberOfChannelsKey : @2,
        AVEncoderBitRateKey : @(std::clamp(settings_.audioBitrateKbps, 96, 320) * 1000),
      };
      audioInput_ = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                                   outputSettings:aac
                                                 sourceFormatHint:pcmFormat_];
      audioInput_.expectsMediaDataInRealTime = YES;

      // Both streams BEFORE writing starts (Pro #286: an audio stream added
      // later silently never exists).
      if (![writer_ canAddInput:videoInput_]) {
        CFRelease(probe);
        CVPixelBufferRelease(black);
        error = "the MP4 writer refused the H.264 stream";
        reset();
        return false;
      }
      [writer_ addInput:videoInput_];
      if (![writer_ canAddInput:audioInput_]) {
        CFRelease(probe);
        CVPixelBufferRelease(black);
        error = "the AAC encoder refused 48 kHz stereo PCM";
        reset();
        return false;
      }
      [writer_ addInput:audioInput_];
      if (![writer_ startWriting]) {
        CFRelease(probe);
        CVPixelBufferRelease(black);
        error = "could not begin writing " + path.string() + ": " + nsErrorText(writer_.error);
        reset();
        return false;
      }
      [writer_ startSessionAtSourceTime:kCMTimeZero];
      started_ = true;
      ++submitted_;  // the probe counts as a submitted video sample until it is appended
      pendingBlack_ = std::make_shared<AvFrame>(black);
      pendingBlack_->preencoded = probe;
    }
    return true;
  }

  FramePtr makeFrame(const std::uint8_t* nv12, std::string& error) override {
    if (!nv12 && pendingBlack_) {
      // The preroll frame open() already encoded.
      FramePtr out = std::move(pendingBlack_);
      pendingBlack_.reset();
      return out;
    }
    CVPixelBufferRef pixels = createPixelBuffer(nv12, error);
    if (!pixels) return nullptr;
    return std::make_shared<AvFrame>(pixels);
  }

  bool writeVideo(const FramePtr& frame, std::int64_t pts100ns, std::int64_t duration100ns,
                  std::string& error) override {
    if (!started_) {
      error = "writer is not open";
      return false;
    }
    auto* av = static_cast<AvFrame*>(frame.get());
    if (!av) {
      error = "no video frame";
      return false;
    }
    const int64_t start = toVideoTicks(pts100ns);
    const int64_t ticks = std::max<int64_t>(1, toVideoTicks(pts100ns + duration100ns) - start);
    if (av->preencoded) {
      CMSampleTimingInfo timing{CMTimeMake(ticks, kVideoTimescale), CMTimeMake(start, kVideoTimescale),
                                CMTimeMake(start, kVideoTimescale)};
      CMSampleBufferRef retimed = nullptr;
      const OSStatus status =
          CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, av->preencoded, 1, &timing, &retimed);
      CFRelease(av->preencoded);
      av->preencoded = nullptr;
      if (status != noErr || !retimed) {
        error = osStatus("could not time the preroll frame", status);
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        encoded_.push_front(retimed);
      }
      return drain(false, error);
    }
    if (!drain(false, error)) return false;
    ++submitted_;
    const OSStatus status = VTCompressionSessionEncodeFrame(session_, av->pixels, CMTimeMake(start, kVideoTimescale),
                                                            CMTimeMake(ticks, kVideoTimescale), nullptr, nullptr,
                                                            nullptr);
    if (status != noErr) {
      --submitted_;
      error = osStatus("the H.264 encoder refused a frame", status);
      return false;
    }
    return true;
  }

  bool writeAudio(const std::int16_t* pcm, std::int64_t startSample, std::int64_t frames,
                  std::string& error) override {
    if (!started_) {
      error = "writer is not open";
      return false;
    }
    if (!drain(false, error)) return false;
    const size_t bytes = static_cast<size_t>(frames) * 4;
    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault,
                                                         nullptr, 0, bytes, kCMBlockBufferAssureMemoryNowFlag, &block);
    if (status == noErr) {
      status = pcm ? CMBlockBufferReplaceDataBytes(pcm, block, 0, bytes) : CMBlockBufferFillDataBytes(0, block, 0, bytes);
    }
    CMSampleBufferRef sample = nullptr;
    if (status == noErr) {
      status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
          kCFAllocatorDefault, block, pcmFormat_, static_cast<CMItemCount>(frames),
          CMTimeMake(startSample, settings_.audioSampleRate), nullptr, &sample);
    }
    if (block) CFRelease(block);
    if (status != noErr || !sample) {
      error = osStatus("could not build an audio sample", status);
      return false;
    }
    // Audio is never dropped (it is sample-counted). The writer holding audio
    // back means video is behind; give the video queue a bounded chance.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!audioInput_.readyForMoreMediaData) {
      if (writer_.status != AVAssetWriterStatusWriting) break;
      if (!drain(false, error)) {
        CFRelease(sample);
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        CFRelease(sample);
        error = "the MP4 writer stopped accepting audio for 2 s";
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const BOOL appended = [audioInput_ appendSampleBuffer:sample];
    CFRelease(sample);
    if (!appended) {
      error = "audio append failed: " + nsErrorText(writer_.error);
      return false;
    }
    return true;
  }

  std::int64_t videoBacklog() const override {
    const auto backlog = submitted_.load() - appended_.load();
    return backlog > 0 ? backlog : 0;
  }

  bool finalize(std::int64_t end100ns, std::string& error) override {
    if (!started_) {
      error = "writer was not open";
      return false;
    }
    bool ok = true;
    @autoreleasepool {
      // End of stream: every submitted frame comes out of VT now (this call
      // returns after the output callbacks ran), then a bounded drain into
      // the file.
      if (session_) {
        const OSStatus status = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
        if (status != noErr) {
          ok = false;
          error = osStatus("the H.264 encoder did not finish", status);
        }
      }
      std::string drainError;
      if (!drain(true, drainError) && ok) {
        ok = false;
        error = drainError;
      }
      [videoInput_ markAsFinished];
      [audioInput_ markAsFinished];
      [writer_ endSessionAtSourceTime:CMTimeMake(end100ns, 10'000'000)];
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [writer_ finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
      }];
      if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC)) != 0) {
        if (ok) error = "the MP4 writer did not finish within 10 s";
        ok = false;
      } else if (writer_.status != AVAssetWriterStatusCompleted) {
        if (ok) error = "the MP4 writer failed to finish: " + nsErrorText(writer_.error);
        ok = false;
      }
      started_ = false;
      writer_ = nil;
      videoInput_ = nil;
      audioInput_ = nil;
    }
    reset();
    return ok;
  }

  void reset() override {
    @autoreleasepool {
      if (session_) {
        VTCompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
      }
      if (writer_ && writer_.status == AVAssetWriterStatusWriting) [writer_ cancelWriting];
      writer_ = nil;
      videoInput_ = nil;
      audioInput_ = nil;
      started_ = false;
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        for (auto* s : encoded_) CFRelease(s);
        encoded_.clear();
      }
      if (pcmFormat_) {
        CFRelease(pcmFormat_);
        pcmFormat_ = nullptr;
      }
      pendingBlack_.reset();
      submitted_ = 0;
      appended_ = 0;
    }
  }

  std::string encoderPath() const override { return encoderPath_; }

 private:
  static void onEncoded(void* refcon, void*, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    auto* self = static_cast<AvIsoEncoder*>(refcon);
    std::lock_guard<std::mutex> lock(self->queueMutex_);
    if (status != noErr) {
      if (self->asyncError_.empty()) self->asyncError_ = osStatus("the H.264 encoder failed a frame", status);
      ++self->appended_;  // it will never reach the file; keep the backlog honest
      return;
    }
    if (!sample || (flags & kVTEncodeInfo_FrameDropped)) {
      if (self->asyncError_.empty()) self->asyncError_ = "the H.264 encoder dropped a frame";
      ++self->appended_;
      return;
    }
    CFRetain(sample);
    self->encoded_.push_back(sample);
  }

  bool createSession(std::string& error) {
    NSDictionary* spec = @{
      (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder :
          settings_.hardwareEncode ? @YES : @NO,
    };
    NSDictionary* source = @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
      (__bridge NSString*)kCVPixelBufferWidthKey : @(settings_.width),
      (__bridge NSString*)kCVPixelBufferHeightKey : @(settings_.height),
      (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, settings_.width, settings_.height,
                                                 kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec,
                                                 (__bridge CFDictionaryRef)source, kCFAllocatorDefault, &onEncoded,
                                                 this, &session_);
    if (status != noErr || !session_) {
      session_ = nullptr;
      error = osStatus(settings_.hardwareEncode ? "no hardware H.264 encoder accepted 1080p NV12"
                                                : "no software H.264 encoder accepted 1080p NV12",
                       status);
      return false;
    }
    const auto set = [&](CFStringRef key, CFTypeRef value) { (void)VTSessionSetProperty(session_, key, value); };
    // Not real time: VT never drops a frame to keep up; IsoMp4Writer's backlog
    // cap is the only place frames are refused, and it counts them.
    set(kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    set(kVTCompressionPropertyKey_AverageBitRate,
        (__bridge CFTypeRef) @(std::max(1, settings_.videoBitrateMbps) * 1'000'000));
    set(kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef) @(settings_.nominalFps));
    set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, (__bridge CFTypeRef) @2);
    set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    status = VTCompressionSessionPrepareToEncodeFrames(session_);
    if (status != noErr) {
      error = osStatus("the H.264 encoder could not prepare", status);
      return false;
    }
    CFBooleanRef hardware = nullptr;
    if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                              kCFAllocatorDefault, &hardware) == noErr &&
        hardware) {
      encoderPath_ = CFBooleanGetValue(hardware) ? "hardware" : "software";
      CFRelease(hardware);
    } else {
      encoderPath_ = settings_.hardwareEncode ? "" : "software";
    }
    return true;
  }

  CVPixelBufferRef createPixelBuffer(const std::uint8_t* nv12, std::string& error) {
    CVPixelBufferRef pixels = nullptr;
    CVPixelBufferPoolRef pool = session_ ? VTCompressionSessionGetPixelBufferPool(session_) : nullptr;
    CVReturn result = pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixels)
                           : CVPixelBufferCreate(kCFAllocatorDefault, settings_.width, settings_.height,
                                                 kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, nullptr, &pixels);
    if (result != kCVReturnSuccess || !pixels) {
      error = "could not allocate a video buffer (CVReturn " + std::to_string(result) + ")";
      return nullptr;
    }
    CVPixelBufferLockBaseAddress(pixels, 0);
    const size_t w = static_cast<size_t>(settings_.width), h = static_cast<size_t>(settings_.height);
    auto* y = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
    auto* uv = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
    for (size_t row = 0; row < h; ++row) {
      if (nv12) std::memcpy(y + row * yStride, nv12 + row * w, w);
      else std::memset(y + row * yStride, 0, w);  // full-range black
    }
    for (size_t row = 0; row < h / 2; ++row) {
      if (nv12) std::memcpy(uv + row * uvStride, nv12 + w * h + row * w, w);
      else std::memset(uv + row * uvStride, 128, w);  // neutral chroma
    }
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    return pixels;
  }

  // Appends every compressed sample the writer will take now. `finishing`:
  // wait (bounded, 2 s) until the queue is empty. Never blocks otherwise: a
  // writer holding video back for audio leaves samples queued, and that queue
  // IS the backlog IsoMp4Writer caps.
  bool drain(bool finishing, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!asyncError_.empty()) {
          error = asyncError_;
          return false;
        }
        while (!encoded_.empty() && videoInput_.readyForMoreMediaData) {
          CMSampleBufferRef sample = encoded_.front();
          encoded_.pop_front();
          const BOOL appended = [videoInput_ appendSampleBuffer:sample];
          CFRelease(sample);
          ++appended_;
          if (!appended) {
            error = "video append failed: " + nsErrorText(writer_.error);
            return false;
          }
        }
        if (!finishing || encoded_.empty()) return true;
      }
      if (writer_.status != AVAssetWriterStatusWriting) {
        error = "the MP4 writer stopped: " + nsErrorText(writer_.error);
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        error = "the MP4 writer did not take the last video samples within 2 s";
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  IsoWriterSettings settings_;
  VTCompressionSessionRef session_ = nullptr;
  AVAssetWriter* writer_ = nil;
  AVAssetWriterInput* videoInput_ = nil;
  AVAssetWriterInput* audioInput_ = nil;
  CMAudioFormatDescriptionRef pcmFormat_ = nullptr;
  bool started_ = false;
  std::shared_ptr<AvFrame> pendingBlack_;
  std::string encoderPath_;

  std::mutex queueMutex_;
  std::deque<CMSampleBufferRef> encoded_;
  std::string asyncError_;
  std::atomic<std::int64_t> submitted_{0};
  std::atomic<std::int64_t> appended_{0};
};

}  // namespace

std::unique_ptr<IsoEncoderBackend> makeIsoEncoderBackend() { return std::make_unique<AvIsoEncoder>(); }

IsoMediaThreadScope::IsoMediaThreadScope() { ok_ = true; }
IsoMediaThreadScope::~IsoMediaThreadScope() = default;

}  // namespace corevideo::edge
