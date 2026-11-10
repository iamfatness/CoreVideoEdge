#include "edge/FeedSender.h"

#include "core/BoundedAsyncLog.h"
#include "edge/FeedSink.h"
#include "edge/IsoAudioTimeline.h"
#include "edge/NdiRuntime.h"
#include "edge/ShmReopen.h"
#include "edge/SrtFeedSink.h"
#include "modules/ZoomEngineClient.h"

#include "engine-ipc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

namespace corevideo::edge {

namespace {

std::int64_t steady100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
             .count() / 100;
}

std::string xmlEscape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// Zoom raw video is requested BT.709 FULL range; NDI's YUV is studio range.
struct RangeLut {
  std::array<std::uint8_t, 256> y{}, c{};
  RangeLut() {
    for (int i = 0; i < 256; ++i) {
      y[static_cast<size_t>(i)] = static_cast<std::uint8_t>(16 + (i * 219 + 127) / 255);
      c[static_cast<size_t>(i)] = static_cast<std::uint8_t>(128 + ((i - 128) * 224 + (i >= 128 ? 127 : -127)) / 255);
    }
  }
};
const RangeLut& lut() {
  static const RangeLut table;
  return table;
}

}  // namespace

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

class NdiSink final : public FeedSink {
 public:
  explicit NdiSink(std::string name) : name_(std::move(name)) {}
  ~NdiSink() override {
    if (api_ && instance_) api_->sendDestroy(instance_);
  }
  bool open(std::string& error) override {
    api_ = ndi::load(error);
    if (!api_) return false;
    ndi::SendCreate create{name_.c_str(), nullptr, false, false};  // frames go out as they arrive
    instance_ = api_->sendCreate(&create);
    if (!instance_) {
      error = "NDI could not create the sender \"" + name_ + "\".";
      return false;
    }
    return true;
  }
  void setGuest(const std::string& name, std::uint32_t id) override {
    metadata_ = "<corevideo_edge guest=\"" + xmlEscape(name) + "\" zoom_id=\"" + std::to_string(id) + "\"/>";
  }
  void video(const std::uint8_t* i420, int w, int h) override {
    const size_t yLen = static_cast<size_t>(w) * h;
    buffer_.resize(yLen + yLen / 2);
    const auto& t = lut();
    for (size_t i = 0; i < yLen; ++i) buffer_[i] = t.y[i420[i]];
    for (size_t i = yLen; i < yLen + yLen / 2; ++i) buffer_[i] = t.c[i420[i]];
    ndi::Video frame{};
    frame.width = w;
    frame.height = h;
    frame.fourcc = ndi::kFourccI420;
    frame.fpsNumerator = 30000;   // informational: clock_video is off, frames carry their own timing
    frame.fpsDenominator = 1000;
    frame.aspect = static_cast<float>(w) / static_cast<float>(h);
    frame.frameFormat = 1;
    frame.timecode = ndi::kTimecodeSynthesize;
    frame.data = buffer_.data();
    frame.stride = w;
    frame.metadata = (++frames_ % 30 == 1) && !metadata_.empty() ? metadata_.c_str() : nullptr;
    api_->sendVideo(instance_, &frame);
  }
  void audio(const float* stereo, int frames) override {
    planar_.resize(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
      planar_[static_cast<size_t>(i)] = stereo[i * 2];
      planar_[static_cast<size_t>(frames + i)] = stereo[i * 2 + 1];
    }
    ndi::Audio a{};
    a.sampleRate = 48000;
    a.channels = 2;
    a.samples = frames;
    a.timecode = ndi::kTimecodeSynthesize;
    a.data = planar_.data();
    a.channelStrideBytes = frames * 4;
    api_->sendAudio(instance_, &a);
  }
  int receivers() override { return api_ && api_->sendConnections ? api_->sendConnections(instance_, 0) : -1; }

 private:
  std::string name_;
  const ndi::Api* api_ = nullptr;
  void* instance_ = nullptr;
  std::vector<std::uint8_t> buffer_;
  std::vector<float> planar_;
  std::string metadata_;
  std::uint64_t frames_ = 0;
};

// ---------------------------------------------------------------------------
// Worker: one routed slot
// ---------------------------------------------------------------------------

struct FeedSender::Worker {
  FeedSlotSpec spec;
  std::string prefix;
  std::unique_ptr<FeedSink> sink;
  std::thread thread;
  std::atomic<bool> running{true};
  mutable std::mutex healthMutex;
  FeedHealth health;

  ~Worker() {
    running = false;
    if (thread.joinable()) thread.join();
  }

  void setState(const std::string& state, const std::string& error = {}) {
    std::lock_guard<std::mutex> lock(healthMutex);
    health.state = state;
    health.error = error;
  }

  void run() {
    std::string error;
    if (!sink->open(error)) {
      setState("failed", error);
      core::nativeLogf("[feeds] slot %d: %s\n", spec.slot, error.c_str());
      return;
    }
    sink->setGuest(spec.guestName, spec.participantId);
    setState("waiting");
    ShmRegion video{}, audio{};
    ShmWatch videoWatch, audioWatch;  // POSIX: reopen a region the engine re-created
    std::uint32_t lastSeq = 0;
    std::uint64_t audioNext = 0;
    std::vector<std::uint8_t> frame;
    std::vector<modules::ZoomEnginePcmAudioChunk> chunks;
    std::vector<float> stereo;
    IsoAudioTimeline timeline(48000);
    const auto start = steady100ns();
    std::uint64_t sent = 0, skipped = 0;
    std::int64_t audioFrames = 0;
    while (running.load()) {
      const auto watchNow = shmWatchNowMs();
      if (video.ptr && videoWatch.shouldReopen(static_cast<const volatile ShmFrameHeader*>(video.ptr)->sequence, watchNow))
        shm_region_destroy(video);
      if (audio.ptr &&
          audioWatch.shouldReopen(static_cast<const volatile ShmAudioRingHeader*>(audio.ptr)->write_counter, watchNow))
        shm_region_destroy(audio);
      if (!video.ptr) {
        const auto name = prefix + spec.sourceUuid;
        if (!shm_region_open_read(video, name, i420_region_capacity(kMaxVideoShmYLen)))
          shm_region_open_read(video, name, sizeof(ShmFrameHeader) + 1920u * 1080u * 3u / 2u);
      }
      if (!audio.ptr && shm_region_open_read(audio, prefix + spec.sourceUuid + "_audio", audio_ring_region_size())) {
        const auto& h = *static_cast<const ShmAudioRingHeader*>(audio.ptr);
        audioNext = h.magic == kAudioRingMagic ? h.write_counter : 0;
      }
      bool did = false;
      if (video.ptr) {
        const auto* h = static_cast<const volatile ShmFrameHeader*>(video.ptr);
        const std::uint32_t before = h->sequence;
        const std::uint32_t w = h->width, ht = h->height;
        const std::uint64_t yLen = static_cast<std::uint64_t>(w) * ht;
        if (before != lastSeq && (before & 1u) == 0 && w >= 2 && ht >= 2 && yLen <= kMaxVideoShmYLen &&
            sizeof(ShmFrameHeader) + yLen * 3 / 2 <= video.size) {
          frame.resize(static_cast<size_t>(yLen * 3 / 2));
          std::memcpy(frame.data(), static_cast<const std::uint8_t*>(video.ptr) + sizeof(ShmFrameHeader), frame.size());
          std::atomic_thread_fence(std::memory_order_acquire);
          if (h->sequence == before) {
            if (lastSeq && before > lastSeq) skipped += (before - lastSeq) / 2 - 1;  // frames Zoom delivered that we never saw
            lastSeq = before;
            sink->video(frame.data(), static_cast<int>(w), static_cast<int>(ht));
            ++sent;
            did = true;
          }
        }
      }
      if (audio.ptr) {
        chunks.clear();
        (void)modules::readZoomEnginePcmAudioRing(audio.ptr, audio.size, audioNext, chunks);
        stereo.clear();
        std::int64_t frames = 0;
        for (const auto& c : chunks) {
          if (c.sampleRate != 48000 || c.channels < 1 || c.channels > 2) continue;
          const auto n = static_cast<std::int64_t>(c.pcm.size()) / c.channels;
          for (std::int64_t i = 0; i < n; ++i) {
            const float l = c.pcm[static_cast<size_t>(i * c.channels)];
            stereo.push_back(l);
            stereo.push_back(c.channels == 2 ? c.pcm[static_cast<size_t>(i * 2 + 1)] : l);
          }
          frames += n;
        }
        const auto now = steady100ns() - start;
        if (frames > 0) {
          const auto placement = timeline.place(now, frames);
          sendSilence(placement.silenceFrames);
          sink->audio(stereo.data(), static_cast<int>(frames));
          audioFrames += placement.silenceFrames + frames;
          did = true;
        }
      }
      {
        // Silence flows even before the engine has an audio ring for this
        // guest: Zoom sends nothing for someone who never speaks, and the
        // engine creates the ring on the first packet (live 2026-11-09: the
        // NDI feeds of silent guests had no audio track at all).
        const auto fill = timeline.catchUp(steady100ns() - start);
        if (fill > 0) { sendSilence(fill); audioFrames += fill; }
      }
      {
        std::lock_guard<std::mutex> lock(healthMutex);
        health.framesSent = sent;
        health.framesSkipped = skipped;
        health.audioSeconds = static_cast<std::uint64_t>(audioFrames / 48000);
        health.receivers = sink->receivers();
        const auto problem = sink->problem();
        if (!problem.empty()) { health.state = "failed"; health.error = problem; }
        else if (sent > 0) { health.state = "sending"; health.error.clear(); }
        health.connected = sink->connected();
      }
      if (!did) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    shm_region_destroy(video);
    shm_region_destroy(audio);
  }

  void sendSilence(std::int64_t frames) {
    static const std::vector<float> zeros(4800 * 2, 0.0f);
    while (frames > 0) {
      const auto n = static_cast<int>(std::min<std::int64_t>(frames, 4800));
      sink->audio(zeros.data(), n);
      frames -= n;
    }
  }
};

// ---------------------------------------------------------------------------

FeedSender::FeedSender() = default;
FeedSender::~FeedSender() { stopAll(); }

std::string FeedSender::configure(FeedMode mode, const SrtSettings& srt, const std::string& nodeName) {
  if (mode == FeedMode::Srt) {
    const auto problem = validateSrt(srt);
    if (!problem.empty()) return problem;
  }
  std::string loadError;
  if (mode == FeedMode::Ndi && !ndi::load(loadError)) return loadError;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = mode != mode_ || srt.host != srt_.host || srt.basePort != srt_.basePort ||
                         srt.latencyMs != srt_.latencyMs || srt.passphrase != srt_.passphrase ||
                         nodeName != nodeName_;
    mode_ = mode;
    srt_ = srt;
    nodeName_ = nodeName;
    modeError_.clear();
    if (changed) workers_.clear();  // ~Worker joins: every feed restarts with the new target
  }
  reconcile();
  return {};
}

void FeedSender::setSlots(const std::string& shmPrefix, std::vector<FeedSlotSpec> slots) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shmPrefix_ = shmPrefix;
    wanted_ = std::move(slots);
  }
  reconcile();
}

void FeedSender::stopAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  workers_.clear();
}

void FeedSender::reconcile() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = workers_.begin(); it != workers_.end();) {
    const auto match = std::find_if(wanted_.begin(), wanted_.end(), [&](const FeedSlotSpec& w) {
      return w.slot == it->first && w.sourceUuid == it->second->spec.sourceUuid &&
             w.participantId == it->second->spec.participantId;
    });
    if (mode_ == FeedMode::Off || match == wanted_.end() || shmPrefix_ != it->second->prefix) it = workers_.erase(it);
    else ++it;
  }
  if (mode_ == FeedMode::Off || shmPrefix_.empty()) return;
  for (const auto& w : wanted_) {
    if (workers_.count(w.slot)) continue;
    auto worker = std::make_unique<Worker>();
    worker->spec = w;
    worker->prefix = shmPrefix_;
    worker->health.slot = w.slot;
    if (mode_ == FeedMode::Ndi) {
      worker->health.target = ndiFeedName(nodeName_, w.slot);
      worker->sink = std::make_unique<NdiSink>(worker->health.target);
    } else {
      worker->health.target = srtUrl(srt_, w.slot, true);
      worker->sink = makeSrtFeedSink(srt_, w.slot);
    }
    worker->health.state = "starting";
    auto* raw = worker.get();
    worker->thread = std::thread([raw] { raw->run(); });
    workers_[w.slot] = std::move(worker);
  }
}

FeedMode FeedSender::mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}
SrtSettings FeedSender::srt() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return srt_;
}
std::string FeedSender::modeError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return modeError_;
}

std::vector<FeedHealth> FeedSender::health() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FeedHealth> out;
  for (const auto& [slot, w] : workers_) {
    std::lock_guard<std::mutex> h(w->healthMutex);
    out.push_back(w->health);
  }
  return out;
}

}  // namespace corevideo::edge
