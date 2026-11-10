#include "edge/PreviewPublisher.h"

#include "core/BoundedAsyncLog.h"
#include "edge/ShmReopen.h"
#include "modules/ZoomEngineClient.h"

#include "engine-ipc.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace corevideo::edge {

namespace {

constexpr int kStride = PreviewPublisher::kWidth * 4;
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kFileBytes = kHeaderBytes + static_cast<std::size_t>(kStride) * PreviewPublisher::kHeight;

std::int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A file-backed shared mapping: the same mechanism on Windows and POSIX, and
// what .NET's MemoryMappedFile.CreateFromFile opens on both.
class MappedFile {
 public:
  ~MappedFile() { close(); }
  bool open(const std::filesystem::path& path, std::size_t bytes) {
    path_ = path;
#if defined(_WIN32)
    file_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr);
    if (!mapping_) { close(); return false; }
    view_ = static_cast<std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (!view_) { close(); return false; }
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd_ < 0) return false;
    if (ftruncate(fd_, static_cast<off_t>(bytes)) != 0) { close(); return false; }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) { close(); return false; }
    view_ = static_cast<std::uint8_t*>(p);
#endif
    bytes_ = bytes;
    std::memset(view_, 0, bytes);
    return true;
  }
  void close() {
#if defined(_WIN32)
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    mapping_ = nullptr;
    file_ = INVALID_HANDLE_VALUE;
#else
    if (view_) munmap(view_, bytes_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    view_ = nullptr;
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);  // unrouted: the file goes away
      path_.clear();
    }
  }
  [[nodiscard]] std::uint8_t* data() const { return view_; }

 private:
  std::filesystem::path path_;
  std::uint8_t* view_ = nullptr;
  std::size_t bytes_ = 0;
#if defined(_WIN32)
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

template <typename T>
void put(std::uint8_t* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(T));
}

}  // namespace

struct PreviewPublisher::Slot {
  PreviewSlot spec;
  MappedFile file;
  ShmRegion video{};
  ShmRegion audio{};
  ShmWatch videoWatch, audioWatch;  // POSIX: reopen a region the engine re-created
  std::uint32_t lastSequence = 0;
  std::uint64_t audioNext = 0;
  bool audioPrimed = false;
  std::uint32_t publishSeq = 0;
  PreviewMeasure measure;
  std::int64_t lastFrameMs = 0;
  std::int64_t windowStartMs = 0;
  std::uint64_t windowFrames = 0;
  float windowPeak = 0.0f;
  std::int64_t audioWindowStartMs = 0;
  bool audioSeenInWindow = false;
  std::vector<std::uint8_t> picture = std::vector<std::uint8_t>(static_cast<std::size_t>(kStride) * kHeight, 0);
  std::vector<modules::ZoomEnginePcmAudioChunk> chunks;
  ~Slot() {
    shm_region_destroy(video);
    shm_region_destroy(audio);
  }
};

PreviewPublisher::PreviewPublisher() = default;
PreviewPublisher::~PreviewPublisher() { stop(); }

std::filesystem::path PreviewPublisher::defaultDir() {
  std::filesystem::path base;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA")) base = std::filesystem::path(local) / "CoreVideoEdge";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / "Library" / "Caches" / "CoreVideoEdge";
#else
  if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".cache" / "corevideo-edge";
#endif
  if (base.empty()) base = std::filesystem::current_path();
#if defined(_WIN32)
  const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long>(getpid());
#endif
  return base / "preview" / std::to_string(pid);
}

bool PreviewPublisher::start(const std::filesystem::path& dir, std::string& error) {
  stop();
  std::error_code ec;
  // Folders left by node processes that no longer run (a crash) are swept;
  // a folder whose files are still open is in use and stays.
#if defined(_WIN32)
  if (dir.has_parent_path()) {
    for (const auto& entry : std::filesystem::directory_iterator(dir.parent_path(), ec)) {
      if (entry.path() != dir) std::filesystem::remove_all(entry.path(), ec);  // in-use files refuse
    }
    ec.clear();
  }
#endif
  std::filesystem::remove_all(dir, ec);
  ec.clear();
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "could not create the preview folder " + dir.string() + " (" + ec.message() + ")";
    return false;
  }
  dir_ = dir;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void PreviewPublisher::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  slots_.clear();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    measures_.clear();
    wanted_.clear();
  }
  if (!dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
}

void PreviewPublisher::setSlots(const std::string& shmPrefix, std::vector<PreviewSlot> slots) {
  std::lock_guard<std::mutex> lock(mutex_);
  shmPrefix_ = shmPrefix;
  wanted_ = std::move(slots);
}

std::map<int, PreviewMeasure> PreviewPublisher::measures() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return measures_;
}

void PreviewPublisher::run() {
  while (running_.load()) {
    std::string prefix;
    std::vector<PreviewSlot> wanted;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      prefix = shmPrefix_;
      wanted = wanted_;
    }
    // Reconcile: drop slots no longer routed (or re-bound to another uuid).
    for (auto it = slots_.begin(); it != slots_.end();) {
      const auto match = std::find_if(wanted.begin(), wanted.end(), [&](const PreviewSlot& w) {
        return w.slot == it->first && w.sourceUuid == it->second->spec.sourceUuid;
      });
      if (match == wanted.end()) it = slots_.erase(it);
      else { it->second->spec.participantId = match->participantId; ++it; }
    }
    for (const auto& w : wanted) {
      if (slots_.count(w.slot)) continue;
      auto slot = std::make_unique<Slot>();
      slot->spec = w;
      if (!slot->file.open(dir_ / ("slot-" + std::to_string(w.slot) + ".cvp"), kFileBytes)) {
        static thread_local std::map<int, bool> reported;
        if (!reported[w.slot]) core::nativeLogf("[preview] could not create the preview file for slot %d\n", w.slot);
        reported[w.slot] = true;
        continue;
      }
      std::uint8_t* h = slot->file.data();
      std::memcpy(h, "CVEP", 4);
      put<std::uint32_t>(h, 4, 1);
      put<std::uint32_t>(h, 12, kWidth);
      put<std::uint32_t>(h, 16, kHeight);
      put<std::uint32_t>(h, 20, kStride);
      put<std::int32_t>(h, 36, -1);
      put<std::int32_t>(h, 40, INT32_MIN);
      slots_[w.slot] = std::move(slot);
    }
    const auto now = steadyMs();
    std::map<int, PreviewMeasure> measures;
    for (auto& [index, slot] : slots_) {
      if (slot->video.ptr &&
          slot->videoWatch.shouldReopen(static_cast<const volatile ShmFrameHeader*>(slot->video.ptr)->sequence, now))
        shm_region_destroy(slot->video);
      if (slot->audio.ptr &&
          slot->audioWatch.shouldReopen(static_cast<const volatile ShmAudioRingHeader*>(slot->audio.ptr)->write_counter, now))
        shm_region_destroy(slot->audio);
      if (!slot->video.ptr) {
        // The real engine allocates every video region at the fixed 1080p
        // capacity; the fake sizes it to the exact frame. Try both.
        const auto name = prefix + slot->spec.sourceUuid;
        if (!shm_region_open_read(slot->video, name, i420_region_capacity(kMaxVideoShmYLen)))
          shm_region_open_read(slot->video, name, sizeof(ShmFrameHeader) + 1920u * 1080u * 3u / 2u);
      }
      if (!slot->audio.ptr &&
          shm_region_open_read(slot->audio, prefix + slot->spec.sourceUuid + "_audio", audio_ring_region_size())) {
        const auto& header = *static_cast<const ShmAudioRingHeader*>(slot->audio.ptr);
        slot->audioNext = header.magic == kAudioRingMagic ? header.write_counter : 0;  // live only
      }
      publishSlot(*slot, now);
      measures[index] = slot->measure;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      measures_ = std::move(measures);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(66));
  }
}

void PreviewPublisher::publishSlot(Slot& slot, std::int64_t nowMs) {
  bool newFrame = false;
  if (slot.video.ptr) {
    const auto* header = static_cast<const volatile ShmFrameHeader*>(slot.video.ptr);
    const std::uint32_t before = header->sequence;
    const std::uint32_t w = header->width, h = header->height;
    const std::uint64_t yLen = static_cast<std::uint64_t>(w) * h;
    if (before != slot.lastSequence && (before & 1u) == 0 && w >= 2 && h >= 2 && yLen <= kMaxVideoShmYLen &&
        sizeof(ShmFrameHeader) + yLen * 3 / 2 <= slot.video.size) {
      const auto* base = static_cast<const std::uint8_t*>(slot.video.ptr) + sizeof(ShmFrameHeader);
      i420ToBgraFit(base, base + yLen, base + yLen + yLen / 4, static_cast<int>(w), static_cast<int>(h),
                    slot.picture.data(), kWidth, kHeight);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (header->sequence == before) {  // not torn
        newFrame = true;
        // The engine's sequence advances by 2 per frame (odd while writing), so
        // the delta counts every frame Zoom delivered, not just the ones this
        // 15 Hz preview happened to sample: the fps is the source's.
        // A lower sequence is a re-created region (POSIX reopen): one frame.
        const std::uint32_t delivered = slot.lastSequence && before > slot.lastSequence ? (before - slot.lastSequence) / 2 : 1;
        slot.lastSequence = before;
        slot.measure.sourceWidth = w;
        slot.measure.sourceHeight = h;
        slot.measure.frames += delivered;
        slot.windowFrames += delivered;
        slot.lastFrameMs = nowMs;
      }
    }
  }
  if (slot.windowStartMs == 0) slot.windowStartMs = nowMs;
  if (nowMs - slot.windowStartMs >= 1000) {
    slot.measure.fpsX100 = static_cast<int>(slot.windowFrames * 100000 / static_cast<std::uint64_t>(nowMs - slot.windowStartMs));
    slot.windowFrames = 0;
    slot.windowStartMs = nowMs;
  }
  // Audio: peak of everything that arrived since the last read, over ~100 ms.
  if (slot.audio.ptr) {
    slot.chunks.clear();
    (void)modules::readZoomEnginePcmAudioRing(slot.audio.ptr, slot.audio.size, slot.audioNext, slot.chunks);
    for (const auto& c : slot.chunks) {
      for (const float s : c.pcm) slot.windowPeak = std::max(slot.windowPeak, std::fabs(s));
      if (!c.pcm.empty()) slot.audioSeenInWindow = true;
    }
  }
  if (slot.audioWindowStartMs == 0) slot.audioWindowStartMs = nowMs;
  if (nowMs - slot.audioWindowStartMs >= 100) {
    if (slot.audioSeenInWindow) {
      slot.measure.hasAudio = true;
      const float peak = std::max(slot.windowPeak, 1e-5f);  // -100 dBFS floor
      slot.measure.audioPeakDbX100 = static_cast<int>(std::lround(20.0 * std::log10(peak) * 100.0));
    } else if (slot.measure.hasAudio) {
      slot.measure.audioPeakDbX100 = -10000;  // measured: nothing arrived (silent guest)
    }
    slot.windowPeak = 0.0f;
    slot.audioSeenInWindow = false;
    slot.audioWindowStartMs = nowMs;
  }

  std::uint8_t* h = slot.file.data();
  if (!h) return;
  // Seqlock: odd while writing.
  auto* seq = reinterpret_cast<volatile std::uint32_t*>(h + 8);
  *seq = ++slot.publishSeq * 2 - 1;
  std::atomic_thread_fence(std::memory_order_release);
  if (newFrame) std::memcpy(h + kHeaderBytes, slot.picture.data(), slot.picture.size());
  put<std::uint32_t>(h, 24, slot.measure.sourceWidth);
  put<std::uint32_t>(h, 28, slot.measure.sourceHeight);
  put<std::uint32_t>(h, 32, static_cast<std::uint32_t>(slot.measure.frames));
  put<std::int32_t>(h, 36, slot.measure.fpsX100);
  put<std::int32_t>(h, 40, slot.measure.hasAudio ? slot.measure.audioPeakDbX100 : INT32_MIN);
  put<std::uint32_t>(h, 44, slot.spec.participantId);
  put<std::int64_t>(h, 48, nowMs);
  put<std::int64_t>(h, 56, slot.lastFrameMs);
  std::atomic_thread_fence(std::memory_order_release);
  *seq = slot.publishSeq * 2;
}

void PreviewPublisher::i420ToBgraFit(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, int srcW,
                                     int srcH, std::uint8_t* bgra, int dstW, int dstH) {
  std::memset(bgra, 0, static_cast<std::size_t>(dstW) * dstH * 4);
  for (std::size_t i = 3; i < static_cast<std::size_t>(dstW) * dstH * 4; i += 4) bgra[i] = 255;
  if (!y || srcW < 2 || srcH < 2) return;
  // Aspect-fit, centred.
  int fitW = dstW, fitH = dstH;
  if (static_cast<long long>(srcW) * dstH > static_cast<long long>(dstW) * srcH)
    fitH = static_cast<int>(static_cast<long long>(dstW) * srcH / srcW);
  else
    fitW = static_cast<int>(static_cast<long long>(dstH) * srcW / srcH);
  fitW = std::max(1, fitW);
  fitH = std::max(1, fitH);
  const int ox = (dstW - fitW) / 2, oy = (dstH - fitH) / 2;
  const int cw = srcW / 2;
  for (int dy = 0; dy < fitH; ++dy) {
    const int sy = static_cast<int>(static_cast<long long>(dy) * srcH / fitH);
    const std::uint8_t* yRow = y + static_cast<std::size_t>(sy) * srcW;
    const std::uint8_t* uRow = u + static_cast<std::size_t>(sy / 2) * cw;
    const std::uint8_t* vRow = v + static_cast<std::size_t>(sy / 2) * cw;
    std::uint8_t* out = bgra + (static_cast<std::size_t>(oy + dy) * dstW + ox) * 4;
    for (int dx = 0; dx < fitW; ++dx) {
      const int sx = static_cast<int>(static_cast<long long>(dx) * srcW / fitW);
      const int Y = yRow[sx];
      const int Cb = uRow[sx / 2] - 128;
      const int Cr = vRow[sx / 2] - 128;
      // BT.709 full range, 16.16 fixed point.
      const int r = Y + ((103206 * Cr) >> 16);
      const int g = Y - ((12276 * Cb + 30679 * Cr) >> 16);
      const int b = Y + ((121608 * Cb) >> 16);
      out[0] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
      out[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
      out[2] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
      out[3] = 255;
      out += 4;
    }
  }
}

}  // namespace corevideo::edge
