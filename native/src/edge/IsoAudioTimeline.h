#pragma once

// Places one ISO source's isolated-audio packets on the recording's shared
// epoch so every stem lines up with every other ISO (and with CoreVideo Pro's
// program, which timestamps against the same kind of clock).
//
// Zoom delivers a guest's isolated audio only while they talk: between bursts
// the packets simply stop. A naive gapless counter would butt the next burst
// against the last one and the stem would run EARLY by every pause. So each
// batch is placed where it belongs in wall time, and the gap is filled with
// silence (CoreVideo Pro ISO spec section 2c; same idea as
// RecordingPtsClock::isoAudioAdvance).
//
// Two differences from Pro's isoAudioAdvance, both against drift:
//  1. A batch read at time `now` ENDS at `now`, so its start is
//     now - batchFrames. Anchoring the start to `now` instead (Pro) turns every
//     late poll into inserted silence that is never taken back, so a guest who
//     talks continuously drifts later by the sum of poll jitter.
//  2. Gaps smaller than `toleranceFrames` are not filled. Packet delivery
//     jitter of a few ms is not a pause in speech; filling it would add
//     silence inside words. The placement error this allows is bounded by the
//     tolerance (default 40 ms, about one video frame) and is corrected by the
//     next real pause.
//
// The timeline never rewinds: Media Foundation requires monotonic audio, and
// content already written cannot be moved.
//
// Pure (caller supplies time in 100 ns ticks relative to the epoch).

#include <cstdint>

namespace corevideo::edge {

class IsoAudioTimeline {
 public:
  explicit IsoAudioTimeline(int sampleRate = 48000, int toleranceMs = 40)
      : rate_(sampleRate > 0 ? sampleRate : 48000),
        toleranceFrames_(static_cast<std::int64_t>(rate_) * (toleranceMs > 0 ? toleranceMs : 0) / 1000) {}

  struct Placement {
    std::int64_t silenceFrames = 0;  // write this much silence first
    std::int64_t startFrame = 0;     // where the real batch begins
  };

  // `nowSinceEpoch100ns`: when the batch was read. `batchFrames`: sample
  // frames (per channel) in the batch. Returns what to write.
  Placement place(std::int64_t nowSinceEpoch100ns, std::int64_t batchFrames) {
    if (batchFrames < 0) batchFrames = 0;
    if (nowSinceEpoch100ns < 0) nowSinceEpoch100ns = 0;
    const std::int64_t nowFrame = nowSinceEpoch100ns / 10'000'000 * rate_ +
                                  (nowSinceEpoch100ns % 10'000'000) * rate_ / 10'000'000;
    std::int64_t expectedStart = nowFrame - batchFrames;
    if (expectedStart < 0) expectedStart = 0;
    Placement out;
    if (expectedStart - written_ > toleranceFrames_) out.silenceFrames = expectedStart - written_;
    out.startFrame = written_ + out.silenceFrames;
    written_ = out.startFrame + batchFrames;
    return out;
  }

  // A guest who is not talking sends nothing, and an audio track that stops
  // advancing makes the MP4 muxer hold every video frame of that ISO (live
  // 2026-11-09: 50 GB, engine killed). So while no packets arrive, the stem is
  // filled with silence up to `holdbackMs` behind real time. The holdback is
  // room for late packets: one that arrives within it is placed exactly; one
  // later than that lands at the end of the silence already written. Returns
  // the silence to write now (0 until the gap exceeds the tolerance, so it is
  // written in chunks, not one sliver per poll).
  [[nodiscard]] std::int64_t catchUp(std::int64_t nowSinceEpoch100ns, int holdbackMs = 200) {
    if (nowSinceEpoch100ns < 0) return 0;
    const std::int64_t nowFrame = nowSinceEpoch100ns / 10'000'000 * rate_ +
                                  (nowSinceEpoch100ns % 10'000'000) * rate_ / 10'000'000;
    const std::int64_t target = nowFrame - static_cast<std::int64_t>(rate_) * holdbackMs / 1000;
    if (target - written_ <= toleranceFrames_) return 0;
    const auto fill = target - written_;
    written_ = target;
    return fill;
  }

  // Silence needed to bring the stem to `endFrame` (the take's common end).
  [[nodiscard]] std::int64_t padTo(std::int64_t endFrame) {
    const auto pad = endFrame > written_ ? endFrame - written_ : 0;
    written_ += pad;
    return pad;
  }

  [[nodiscard]] std::int64_t writtenFrames() const { return written_; }
  [[nodiscard]] int sampleRate() const { return rate_; }

 private:
  int rate_;
  std::int64_t toleranceFrames_;
  std::int64_t written_ = 0;
};

}  // namespace corevideo::edge
