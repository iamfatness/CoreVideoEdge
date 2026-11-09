#include "edge/IsoSourceRecorder.h"

#include "core/BoundedAsyncLog.h"
#include "edge/IsoAudioTimeline.h"
#include "edge/FastConform.h"
#include "edge/SteadyClock.h"
#include "modules/IsoFrameConform.h"
#include "modules/ZoomEngineClient.h"

#include "engine-ipc.h"
#include "iso-video-ring.h"

#include <mfapi.h>
#include <objbase.h>

#include <chrono>
#include <vector>

namespace corevideo::edge {
namespace {

constexpr auto kPoll = std::chrono::milliseconds(5);


struct ComMfScope {
  bool com = false;
  bool mf = false;
  ComMfScope() {
    com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~ComMfScope() {
    if (mf) MFShutdown();
    if (com) CoUninitialize();
  }
};

std::int64_t msSince(std::int64_t then100ns, std::int64_t now100ns) {
  return then100ns <= 0 ? -1 : (now100ns - then100ns) / 10'000;
}

}  // namespace

IsoSourceRecorder::~IsoSourceRecorder() {
  if (thread_.joinable()) stop(steadyNow100ns());
}

bool IsoSourceRecorder::start(const IsoSourceConfig& config, std::string& error) {
  config_ = config;
  participantId_.store(config.participantId);
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    health_ = {};
    health_.index = config.index;
    health_.slot = config.slot;
    health_.displayName = config.displayName;
    health_.participantId = config.participantId;
    health_.path = config.path.string();
    health_.state = "opening";
  }
  stopRequested_ = false;
  begun_ = false;
  thread_ = std::thread([this] { run(); });
  std::unique_lock<std::mutex> lock(openMutex_);
  openCv_.wait(lock, [this] { return openDone_; });
  if (!openOk_) {
    error = openError_;
    lock.unlock();
    thread_.join();
    return false;
  }
  return true;
}

void IsoSourceRecorder::begin(std::int64_t epoch100ns) {
  epoch100ns_.store(epoch100ns);
  begun_.store(true);
}

void IsoSourceRecorder::stop(std::int64_t stop100ns) {
  stopAt100ns_.store(stop100ns);
  stopRequested_.store(true);
  if (thread_.joinable()) thread_.join();
}

void IsoSourceRecorder::setParticipant(std::uint32_t participantId) {
  participantId_.store(participantId);
  std::lock_guard<std::mutex> lock(healthMutex_);
  health_.participantId = participantId;
}

IsoSourceHealth IsoSourceRecorder::health() const {
  std::lock_guard<std::mutex> lock(healthMutex_);
  return health_;
}

void IsoSourceRecorder::setWarning(const std::string& text) {
  std::lock_guard<std::mutex> lock(healthMutex_);
  if (health_.warning.empty()) health_.warning = text;
  core::nativeLogf("[iso] %s: %s\n", config_.displayName.c_str(), text.c_str());
}

void IsoSourceRecorder::run() {
  ComMfScope scope;
  IsoMp4Writer writer;
  std::string error;
  bool opened = scope.com && scope.mf && writer.open(config_.path, config_.writer, error);
  if (!scope.com || !scope.mf) error = "Media Foundation is not available on this machine";
  if (!opened && config_.writer.hardwareEncode && scope.mf) {
    // A GPU with no free encoder session (consumer cards cap them) or no H.264
    // MFT at all: fall back to the software encoder for THIS file, and say so
    // through the measured encoder path rather than pretending.
    core::nativeLogf("[iso] %s: hardware encoder open failed (%s); retrying in software\n",
                     config_.displayName.c_str(), error.c_str());
    auto settings = config_.writer;
    settings.hardwareEncode = false;
    error.clear();
    opened = writer.open(config_.path, settings, error);
  }
  {
    std::lock_guard<std::mutex> lock(openMutex_);
    openDone_ = true;
    openOk_ = opened;
    openError_ = opened ? std::string() : "ISO " + config_.displayName + ": " + error;
  }
  openCv_.notify_all();
  if (!opened) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    health_.state = "failed";
    health_.warning = openError_;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    health_.state = "recording";
    health_.encoder = writer.encoderPath();
  }

  // Hold until every file of the take is open and the epoch is set.
  while (!begun_.load() && !stopRequested_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const std::int64_t epoch = begun_.load() ? epoch100ns_.load() : steadyNow100ns();

  const auto videoName = config_.shmPrefix + config_.slotUuid + "-iso";
  const auto audioName = config_.shmPrefix + config_.slotUuid + "_audio";
  ShmRegion videoRing, audioRing;
  std::uint32_t videoNext = 0;
  std::uint64_t audioNext = 0;
  bool audioSkipBacklog = true;  // a ring that already exists at start holds pre-take audio

  IsoAudioTimeline timeline(config_.writer.audioSampleRate);
  std::vector<std::uint8_t> nv12;
  FastConform conform;
  std::vector<float> stereo;
  std::vector<modules::ZoomIsoVideoArrival> arrivals;
  std::vector<modules::ZoomEnginePcmAudioChunk> chunks;
  bool failed = false;
  std::int64_t lastVideo100ns = 0, lastAudio100ns = 0, lastVideoQueued100ns = 0;
  constexpr std::int64_t kHeldRepeat100ns = 10'000'000;  // 1 s
  std::int64_t realAudio = 0, silenceAudio = 0, lostVideo = 0, lostAudio = 0;
  std::uint32_t lastW = 0, lastH = 0;
  bool warnedFormat = false;

  // Per-5 s diagnostics (live 2026-11-09: ~6 frames/s lost on 5 of 6 ISOs,
  // cause unknown). Where the recorder thread's time goes, and whether loss is
  // ring overwrite (reader too slow) or torn copies (writer lapped a read).
  struct Window {
    std::int64_t arrivals = 0, overwritten = 0, lostAtStart = 0;
    std::int64_t read100ns = 0, conform100ns = 0, write100ns = 0, worstDrain100ns = 0, worstGap100ns = 0;
  } stats;
  std::int64_t windowStart = steadyNow100ns(), lastDrainEnd = 0;
  const auto drain = [&]() {
    const auto now = steadyNow100ns();
    if (lastDrainEnd) stats.worstGap100ns = std::max(stats.worstGap100ns, now - lastDrainEnd);
    // ---- video: every arrival since the last poll -------------------------
    if (videoRing.ptr) {
      const auto& header = *static_cast<const zoom_iso::Header*>(videoRing.ptr);
      // The engine re-creates a target's ring when a slot is re-bound to a
      // rejoined guest (same name, same section, counter back to zero).
      if (zoom_iso::load(header.epoch) == config_.isoRingEpoch && zoom_iso::load(header.written) < videoNext)
        videoNext = 0;
      arrivals.clear();
      modules::ZoomIsoRingReadEvidence evidence;
      const auto readStart = steadyNow100ns();
      lostVideo += static_cast<std::int64_t>(modules::readZoomEngineIsoVideoRing(
          videoRing.ptr, videoRing.size, config_.isoRingEpoch, videoNext, config_.slotUuid,
          participantId_.load(), false, arrivals, &evidence));
      stats.read100ns += steadyNow100ns() - readStart;
      stats.overwritten += static_cast<std::int64_t>(evidence.overwritten);
      stats.arrivals += static_cast<std::int64_t>(arrivals.size());
      for (auto& arrival : arrivals) {
        const auto pts = arrival.captured100ns - epoch;
        if (pts < 0) continue;  // captured before the take started
        const auto& frame = arrival.frame;
        if (frame.i420.empty() || frame.i420Width == 0 || frame.i420Height == 0) { ++lostVideo; continue; }
        lastW = frame.i420Width;
        lastH = frame.i420Height;
        lastVideo100ns = now;
        lastVideoQueued100ns = now;
        if (failed) continue;
        const auto conformStart = steadyNow100ns();
        conform.run(frame.i420.data(), static_cast<int>(frame.i420Width), static_cast<int>(frame.i420Height),
                    config_.writer.width, config_.writer.height, nv12);
        const auto writeStart = steadyNow100ns();
        stats.conform100ns += writeStart - conformStart;
        const bool wrote = writer.writeVideo(nv12.data(), pts, error);
        stats.write100ns += steadyNow100ns() - writeStart;
        if (!wrote) {
          failed = true;
          setWarning("ISO writer stopped writing video: " + error);
        }
      }
    }
    // ---- audio: isolated stem, placed on the shared epoch -----------------
    if (audioRing.ptr) {
      chunks.clear();
      lostAudio += static_cast<std::int64_t>(
          modules::readZoomEnginePcmAudioRing(audioRing.ptr, audioRing.size, audioNext, chunks));
      std::int64_t frames = 0;
      stereo.clear();
      for (const auto& chunk : chunks) {
        if (chunk.sampleRate != config_.writer.audioSampleRate || chunk.channels < 1 || chunk.channels > 2) {
          if (!warnedFormat) {
            warnedFormat = true;
            setWarning("audio arrived as " + std::to_string(chunk.sampleRate) + " Hz x" +
                       std::to_string(chunk.channels) + "; this ISO only records 48 kHz mono/stereo, so it is dropped");
          }
          continue;
        }
        const auto n = static_cast<std::int64_t>(chunk.pcm.size()) / chunk.channels;
        for (std::int64_t i = 0; i < n; ++i) {
          const float l = chunk.pcm[static_cast<size_t>(i * chunk.channels)];
          const float r = chunk.channels == 2 ? chunk.pcm[static_cast<size_t>(i * 2 + 1)] : l;
          stereo.push_back(l);
          stereo.push_back(r);
        }
        frames += n;
      }
      if (frames > 0) {
        lastAudio100ns = now;
        const auto placement = timeline.place(now - epoch, frames);
        if (!failed && placement.silenceFrames > 0 && !writer.writeSilence(placement.silenceFrames, error)) {
          failed = true;
          setWarning("ISO writer stopped writing audio: " + error);
        }
        if (!failed && !writer.writeAudio(stereo.data(), frames, error)) {
          failed = true;
          setWarning("ISO writer stopped writing audio: " + error);
        }
        silenceAudio += placement.silenceFrames;
        realAudio += frames;
      }
    }
    // ---- keep both tracks moving (the muxer holds one while the other waits)
    if (!failed) {
      const auto silence = timeline.catchUp(now - epoch);
      if (silence > 0) {
        if (!writer.writeSilence(silence, error)) {
          failed = true;
          setWarning("ISO writer stopped writing audio: " + error);
        }
        silenceAudio += silence;
      }
      // Camera stalled (off, frozen, or never started): repeat the held frame
      // once a second so the video track advances.
      if (!failed && now - std::max(lastVideoQueued100ns, epoch) > kHeldRepeat100ns) {
        if (!writer.repeatHeldVideo(now - epoch, error)) {
          failed = true;
          setWarning("ISO writer stopped writing video: " + error);
        }
        lastVideoQueued100ns = now;
      }
    }
    const auto end = steadyNow100ns();
    stats.worstDrain100ns = std::max(stats.worstDrain100ns, end - now);
    lastDrainEnd = end;
    if (end - windowStart >= 50'000'000) {
      const auto ms = [](std::int64_t t) { return static_cast<double>(t) / 10'000.0; };
      core::nativeLogf("[iso-diag] %s src=%ux%u arrivals=%lld lost=%lld (overwritten=%lld torn=%lld) "
                       "read=%.0fms conform=%.0fms write=%.0fms worstDrain=%.1fms worstGap=%.1fms backlog=%lld\n",
                       config_.displayName.c_str(), lastW, lastH, static_cast<long long>(stats.arrivals),
                       static_cast<long long>(lostVideo - stats.lostAtStart), static_cast<long long>(stats.overwritten),
                       static_cast<long long>(lostVideo - stats.lostAtStart - stats.overwritten),
                       ms(stats.read100ns), ms(stats.conform100ns), ms(stats.write100ns),
                       ms(stats.worstDrain100ns), ms(stats.worstGap100ns),
                       static_cast<long long>(writer.videoBacklog()));
      stats = Window{};
      stats.lostAtStart = lostVideo;
      windowStart = end;
    }
    std::lock_guard<std::mutex> lock(healthMutex_);
    health_.videoFrames = writer.videoFrames();
    health_.prerollFrames = writer.prerollFrames();
    health_.heldRepeats = writer.heldRepeats();
    health_.videoDropped = writer.videoDropped();
    health_.encoderBacklogPeak = writer.videoBacklogPeak();
    health_.cadenceRepeats = writer.cadenceRepeats();
    health_.cadenceSkipped = writer.cadenceSkipped();
    health_.videoLost = lostVideo;
    health_.audioSamples = realAudio;
    health_.silenceSamples = silenceAudio;
    health_.audioLostPackets = lostAudio;
    health_.lastWidth = lastW;
    health_.lastHeight = lastH;
    health_.msSinceVideo = msSince(lastVideo100ns, now);
    health_.msSinceAudio = msSince(lastAudio100ns, now);
    if (failed) health_.state = "failed";
  };

  while (!stopRequested_.load()) {
    // The engine creates a ring on the first frame/packet after it is armed.
    // The video ring holds only 4 frames, so look for it on every poll until
    // it exists (OpenFileMapping on a missing name is a cheap kernel miss).
    if (!videoRing.ptr || !audioRing.ptr) {
      if (!videoRing.ptr) shm_region_open_read(videoRing, videoName, zoom_iso::bytes);
      if (!audioRing.ptr && shm_region_open_read(audioRing, audioName, audio_ring_region_size())) {
        if (audioSkipBacklog) {
          const auto& header = *static_cast<const ShmAudioRingHeader*>(audioRing.ptr);
          audioNext = header.magic == kAudioRingMagic ? header.write_counter : 0;
        }
      }
      audioSkipBacklog = false;  // a ring that appears later was born during the take
    }
    drain();
    std::this_thread::sleep_for(kPoll);
  }
  drain();
  {
    std::lock_guard<std::mutex> lock(healthMutex_);
    if (health_.state != "failed") health_.state = "finalizing";
  }
  const auto stopPts = stopAt100ns_.load() - epoch;
  std::string finalizeError;
  const bool finalized = writer.finalize(stopPts > 0 ? stopPts : 0, finalizeError);
  shm_region_destroy(videoRing);
  shm_region_destroy(audioRing);
  std::lock_guard<std::mutex> lock(healthMutex_);
  health_.videoFrames = writer.videoFrames();
  health_.prerollFrames = writer.prerollFrames();
  health_.heldRepeats = writer.heldRepeats();
  health_.videoDropped = writer.videoDropped();
  health_.encoderBacklogPeak = writer.videoBacklogPeak();
    health_.cadenceRepeats = writer.cadenceRepeats();
    health_.cadenceSkipped = writer.cadenceSkipped();
  health_.silenceSamples = writer.audioSamplesWritten() - realAudio;
  health_.bytes = writer.bytesOnDisk();
  if (!finalized) {
    health_.state = "failed";
    if (health_.warning.empty()) health_.warning = "ISO file did not finalize: " + finalizeError;
  } else if (!failed) {
    health_.state = "finalized";
  }
}

}  // namespace corevideo::edge
