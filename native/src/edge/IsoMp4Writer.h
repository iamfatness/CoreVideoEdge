#pragma once

// One ISO file: H.264 video (NV12 input) + AAC stereo audio in a fragmented
// MP4, written with Media Foundation.
//
// A focused subset of CoreVideo Pro's Mp4Writer
// (native/src/modules/MediaFoundationEncoderAdapter.cpp), keeping the rules Pro
// learned the hard way:
//  - BT.709 full-range NV12 in and out (Zoom raw I420 is requested that way);
//  - the AAC stream is added BEFORE BeginWriting (#286: added later it fails and
//    the ISO silently has no audio track);
//  - fragmented MP4 (MFCreateFMPEG4MediaSink, 1 s fragments) so a crash or power
//    loss loses only the in-flight tail;
//  - Sink Writer throttling OFF (#1030: with it on, a stalled video stream
//    blocks the next audio WriteSample forever);
//  - audio PTS are sample-counted (gapless AAC), silence is written explicitly;
//  - explicit end-of-segment + bounded drain before Finalize;
//  - every failure is returned as text, never swallowed.
//
// Edge differences:
//  - the writer always opens at the subscribed size (1920x1080). Pro opens at
//    the first frame's size, which pins a file to Zoom's 640x360 ramp frame if
//    that arrives first; the caller conforms every frame to this size instead.
//  - video is variable-frame-rate by construction: each sample is held until
//    the next one arrives so its duration is the real gap. A guest who turns
//    their camera off is a long last frame, not invented repeats.
//  - a black frame at t=0 pins the video track to the shared epoch (Pro does
//    the same for a late first frame) and is counted as preroll, not as a frame.
//  - the MP4 muxer interleaves by time: a track that stops advancing makes it
//    hold every sample of the OTHER track. With throttling off that hold is
//    unbounded (live 2026-11-09: silent guests -> 50 GB, engine killed). So
//    the caller keeps both tracks moving (silence for audio, repeatHeldVideo
//    for a stalled camera) and writeVideo refuses to queue past a hard
//    backlog, counting each refused frame as dropped.
//
// Single-threaded: all calls from the owning recorder thread, which must have
// initialized COM (MTA) and MFStartup.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace corevideo::edge {

struct IsoWriterSettings {
  int width = 1920;
  int height = 1080;
  int nominalFps = 30;
  int videoBitrateMbps = 18;   // Pro's ISO default
  int audioBitrateKbps = 192;
  int audioSampleRate = 48000;
  bool hardwareEncode = true;
  int maxVideoBacklog = 60;    // samples (2 s at 30 fps, ~190 MB at 1080p NV12)
};

class IsoMp4Writer {
 public:
  IsoMp4Writer() = default;
  ~IsoMp4Writer();
  IsoMp4Writer(const IsoMp4Writer&) = delete;
  IsoMp4Writer& operator=(const IsoMp4Writer&) = delete;

  // Creates the file and begins writing. On false, `error` says why and the
  // file may not exist.
  bool open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error);

  // NV12 at exactly settings.width x settings.height. `pts100ns` is time since
  // the shared epoch; must increase (a non-increasing value is nudged +1 tick).
  // A frame is DROPPED (returns true, counted in videoDropped) when the encoder
  // already holds more than maxVideoBacklog samples.
  bool writeVideo(const std::uint8_t* nv12, std::int64_t pts100ns, std::string& error);

  // Camera stalled: re-queue the held frame at `pts100ns` so the video track
  // keeps advancing (counted in heldRepeats, never as a frame). No-op when
  // pts100ns is not after the held frame.
  bool repeatHeldVideo(std::int64_t pts100ns, std::string& error);

  // Samples written to the Sink Writer that it has not yet passed to the file.
  [[nodiscard]] std::int64_t videoBacklog() const;

  // Interleaved stereo float [-1, 1], appended at the next sample position.
  bool writeAudio(const float* stereo, std::int64_t frames, std::string& error);
  bool writeSilence(std::int64_t frames, std::string& error);

  // Flushes the held video sample, brings video and audio to one common end
  // (at least `minimumEnd100ns`), and finalizes the container.
  bool finalize(std::int64_t minimumEnd100ns, std::string& error);

  [[nodiscard]] bool writing() const { return writing_; }
  [[nodiscard]] std::int64_t videoFrames() const { return videoFrames_; }        // real frames only
  [[nodiscard]] std::int64_t prerollFrames() const { return prerollFrames_; }
  [[nodiscard]] std::int64_t heldRepeats() const { return heldRepeats_; }
  [[nodiscard]] std::int64_t videoDropped() const { return videoDropped_; }
  [[nodiscard]] std::int64_t videoBacklogPeak() const { return backlogPeak_; }
  [[nodiscard]] std::int64_t audioSamplesWritten() const { return audioSamples_; }  // includes silence
  [[nodiscard]] std::int64_t videoEnd100ns() const;
  [[nodiscard]] std::int64_t audioEnd100ns() const;
  [[nodiscard]] const std::string& encoderPath() const { return encoderPath_; }  // "hardware"|"software"|""
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::int64_t bytesOnDisk() const;

 private:
  bool writeVideoSample(IMFSample* sample, std::string& error);
  bool queueVideoBuffer(IMFMediaBuffer* buffer, std::int64_t pts100ns, bool preroll, std::string& error);
  bool flushPendingVideo(std::int64_t end100ns, std::string& error);
  bool writePcm16(const std::int16_t* samples, std::int64_t frames, std::string& error);
  void detectEncoderPath();
  void reset();

  std::filesystem::path path_;
  IsoWriterSettings settings_;
  Microsoft::WRL::ComPtr<IMFByteStream> byteStream_;
  Microsoft::WRL::ComPtr<IMFMediaSink> mediaSink_;
  Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_;
  DWORD videoStream_ = 0;
  DWORD audioStream_ = 1;
  bool writing_ = false;
  std::int64_t frameDuration100ns_ = 333'333;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> pendingBuffer_;
  std::int64_t pendingPts100ns_ = -1;
  bool pendingIsPreroll_ = false;
  bool pendingIsRepeat_ = false;
  std::int64_t heldRepeats_ = 0;
  std::int64_t videoDropped_ = 0;
  std::int64_t backlogPeak_ = 0;
  std::int64_t lastVideoPts100ns_ = -1;
  std::int64_t videoEnd100ns_ = 0;
  std::int64_t videoFrames_ = 0;
  std::int64_t prerollFrames_ = 0;

  std::int64_t audioSamples_ = 0;
  std::vector<std::int16_t> pcm16_;
  std::string encoderPath_;
};

}  // namespace corevideo::edge
