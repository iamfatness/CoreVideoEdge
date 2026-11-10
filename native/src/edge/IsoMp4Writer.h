#pragma once

// One ISO file: H.264 video (NV12 input) + AAC stereo audio in a fragmented
// MP4. The timing rules live here and are the same on every platform; the
// encoder/muxer is the platform backend (IsoEncoder.h): Media Foundation on
// Windows, VideoToolbox + AVAssetWriter on macOS.
//
// A focused subset of CoreVideo Pro's Mp4Writer
// (native/src/modules/MediaFoundationEncoderAdapter.cpp), keeping the rules Pro
// learned the hard way:
//  - BT.709 full-range NV12 in and out (Zoom raw I420 is requested that way);
//  - the AAC stream is added BEFORE writing begins (#286: added later it fails
//    and the ISO silently has no audio track);
//  - fragmented MP4 (1 s fragments) so a crash or power loss loses only the
//    in-flight tail;
//  - no muxer throttling (#1030: with it on, a stalled video stream blocks the
//    next audio write forever);
//  - audio PTS are sample-counted (gapless AAC), silence is written explicitly;
//  - explicit end-of-stream + bounded drain before finalize;
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
// Single-threaded: all calls from the owning recorder thread, which must hold
// an IsoMediaThreadScope (Windows: COM MTA + MFStartup).

#include "edge/IsoEncoder.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::edge {

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

  // Samples written to the encoder that it has not yet passed to the file.
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
  // Constant-rate mode only: grid slots filled by repeating the previous frame,
  // and arrivals replaced by a later one in the same slot.
  [[nodiscard]] std::int64_t cadenceRepeats() const { return cadenceRepeats_; }
  [[nodiscard]] std::int64_t cadenceSkipped() const { return cadenceSkipped_; }
  [[nodiscard]] std::int64_t audioSamplesWritten() const { return audioSamples_; }  // includes silence
  [[nodiscard]] std::int64_t videoEnd100ns() const;
  [[nodiscard]] std::int64_t audioEnd100ns() const;
  [[nodiscard]] const std::string& encoderPath() const { return encoderPath_; }  // "hardware"|"software"|""
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::int64_t bytesOnDisk() const;

 private:
  using Frame = IsoEncoderBackend::FramePtr;
  bool queueVideoFrame(Frame frame, std::int64_t pts100ns, bool preroll, std::string& error);
  bool flushPendingVideo(std::int64_t end100ns, std::string& error);
  bool writePcm16(const std::int16_t* samples, std::int64_t frames, std::string& error);
  void reset();

  std::filesystem::path path_;
  IsoWriterSettings settings_;
  std::unique_ptr<IsoEncoderBackend> backend_;
  bool writing_ = false;
  std::int64_t frameDuration100ns_ = 333'333;

  Frame pendingFrame_;
  std::int64_t pendingPts100ns_ = -1;
  bool pendingIsPreroll_ = false;
  bool pendingIsRepeat_ = false;
  bool pendingIsCadence_ = false;  // a constant-rate grid fill, not a camera stall
  std::int64_t heldRepeats_ = 0;
  std::int64_t videoDropped_ = 0;
  std::int64_t backlogPeak_ = 0;
  std::int64_t cadenceRepeats_ = 0;
  std::int64_t cadenceSkipped_ = 0;
  std::int64_t pendingIndex_ = -1;  // constant-rate grid slot of the held frame
  [[nodiscard]] bool constantRate() const { return settings_.constantFps > 0; }
  [[nodiscard]] std::int64_t gridIndex(std::int64_t pts100ns) const;
  [[nodiscard]] std::int64_t gridPts(std::int64_t index) const;
  bool fillGridTo(std::int64_t index, std::string& error, bool stall = false);  // repeat held frame up to index-1
  std::int64_t lastVideoPts100ns_ = -1;
  std::int64_t videoEnd100ns_ = 0;
  std::int64_t videoFrames_ = 0;
  std::int64_t prerollFrames_ = 0;

  std::int64_t audioSamples_ = 0;
  std::vector<std::int16_t> pcm16_;
  std::string encoderPath_;
};

}  // namespace corevideo::edge
