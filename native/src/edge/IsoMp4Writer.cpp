#include "edge/IsoMp4Writer.h"

#include "core/BoundedAsyncLog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace corevideo::edge {

IsoMp4Writer::~IsoMp4Writer() {
  if (writing_) {
    std::string ignored;
    (void)finalize(0, ignored);
  }
  reset();
}

void IsoMp4Writer::reset() {
  pendingFrame_.reset();
  if (backend_) backend_->reset();
  backend_.reset();
  writing_ = false;
}

bool IsoMp4Writer::open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error) {
  reset();
  path_ = path;
  settings_ = settings;
  settings_.width = std::max(2, settings.width & ~1);
  settings_.height = std::max(2, settings.height & ~1);
  settings_.nominalFps = std::max(1, settings.nominalFps);
  // A constant-rate take declares its real rate (60 fps needs H.264 level 4.2
  // and twice the bits of 30; review 2026-11-09).
  if (settings.constantFps == 30 || settings.constantFps == 60) settings_.nominalFps = settings.constantFps;
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

  backend_ = makeIsoEncoderBackend();
  if (!backend_ || !backend_->open(path_, settings_, error)) {
    if (error.empty()) error = "the ISO encoder could not be created";
    reset();
    return false;
  }
  writing_ = true;
  encoderPath_ = backend_->encoderPath();

  // Pin the video track to the epoch with one black frame at t=0. Its duration
  // becomes the gap to the first real frame.
  auto black = backend_->makeFrame(nullptr, error);
  if (!black) {
    if (error.empty()) error = "could not allocate the preroll frame";
    reset();
    return false;
  }
  if (!queueVideoFrame(std::move(black), 0, true, error)) {
    reset();
    return false;
  }
  return true;
}

// Holds `frame` until the next one arrives, so every sample's duration is the
// real gap to its successor (VFR). The previously held sample is written now.
bool IsoMp4Writer::queueVideoFrame(Frame frame, std::int64_t pts100ns, bool preroll, std::string& error) {
  if (pendingFrame_ && !flushPendingVideo(pts100ns, error)) return false;
  pendingFrame_ = std::move(frame);
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
  while (pendingFrame_ && pendingIndex_ + 1 < index) {
    const Frame held = pendingFrame_;
    const auto next = pendingIndex_ + 1;
    if (!flushPendingVideo(gridPts(next), error)) return false;
    pendingFrame_ = held;
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
  return backend_ ? backend_->videoBacklog() : 0;
}

bool IsoMp4Writer::repeatHeldVideo(std::int64_t pts100ns, std::string& error) {
  if (!writing_ || !pendingFrame_ || pts100ns <= pendingPts100ns_) return true;
  if (videoBacklog() > settings_.maxVideoBacklog) return true;  // the held frame just runs longer
  if (constantRate()) return fillGridTo(gridIndex(pts100ns) + 1, error, true);  // the grid IS the repeat
  const Frame held = pendingFrame_;
  if (!flushPendingVideo(pts100ns, error)) return false;
  pendingFrame_ = held;
  pendingPts100ns_ = pts100ns;
  pendingIsPreroll_ = false;
  pendingIsRepeat_ = true;
  pendingIsCadence_ = false;
  lastVideoPts100ns_ = pts100ns;
  return true;
}

bool IsoMp4Writer::flushPendingVideo(std::int64_t end100ns, std::string& error) {
  if (!pendingFrame_) return true;
  const std::int64_t duration = std::max<std::int64_t>(1, end100ns - pendingPts100ns_);
  if (!backend_->writeVideo(pendingFrame_, pendingPts100ns_, duration, error)) return false;
  videoEnd100ns_ = pendingPts100ns_ + duration;
  if (pendingIsPreroll_) ++prerollFrames_;
  else if (pendingIsRepeat_ && pendingIsCadence_) ++cadenceRepeats_;
  else if (pendingIsRepeat_) ++heldRepeats_;
  else ++videoFrames_;
  pendingFrame_.reset();
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
    pts100ns = lastVideoPts100ns_ + 1;  // encoders need strictly increasing
  }
  // Hard bound on memory: never queue past the backlog cap. A dropped frame
  // is counted and the held frame simply runs longer (VFR).
  const auto backlog = videoBacklog();
  backlogPeak_ = std::max(backlogPeak_, backlog);
  if (backlog > settings_.maxVideoBacklog) {
    ++videoDropped_;
    return true;
  }
  auto frame = backend_->makeFrame(nv12, error);
  if (!frame) {
    if (error.empty()) error = "could not allocate a video buffer";
    return false;
  }
  if (constantRate()) {
    if (pendingFrame_ && index <= pendingIndex_) {
      // Same grid slot (or a late arrival): the newest picture wins the slot.
      if (!pendingIsPreroll_ && !pendingIsRepeat_) ++cadenceSkipped_;
      pendingFrame_ = std::move(frame);
      pendingIsPreroll_ = false;
      pendingIsRepeat_ = false;
      return true;
    }
    if (!fillGridTo(index, error)) return false;
  }
  return queueVideoFrame(std::move(frame), pts100ns, false, error);
}

bool IsoMp4Writer::writePcm16(const std::int16_t* samples, std::int64_t frames, std::string& error) {
  if (frames <= 0) return true;
  // Sample-counted: the backend places the block at the running total, never
  // at a per-block rounded time, so the track has no cumulative drift (Pro's
  // rule).
  if (!backend_->writeAudio(samples, audioSamples_, frames, error)) return false;
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
  return pendingFrame_ ? std::max(videoEnd100ns_, pendingPts100ns_ + frameDuration100ns_) : videoEnd100ns_;
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

  // Signal end of stream and give the encoder a bounded chance to drain before
  // the container writes its index. Never an unbounded wait (backend rule).
  std::string finalizeError;
  if (!backend_->finalize(std::max(end, audioEnd100ns()), finalizeError) && ok) {
    ok = false;
    firstError = finalizeError;
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
