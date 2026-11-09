#pragma once

// Live preview of every routed slot for the operator app's Monitor screen.
//
// The node already owns the Zoom engine's shared memory; the operator app must
// not. So the node publishes a small, cheap picture per routed slot into a
// memory-mapped file the app reads (format: docs/PROTOCOL.md "Preview frames").
// File mapping (not a named section) because it is the one mechanism that is
// the same on Windows and macOS for both C++ and .NET.
//
// One thread at ~15 Hz. Per slot: if the engine's frame sequence moved (and is
// not mid-write), downscale straight from shared memory into 480x270 BGRA
// (letterboxed, BT.709 full range - what Edge subscribes), re-check the
// sequence (a torn read is discarded), count frames for a measured fps, and
// read the guest's isolated audio ring with its own cursor for a peak level.
// Everything published is measured; nothing is estimated.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::edge {

struct PreviewSlot {
  int slot = 0;
  std::string sourceUuid;
  std::uint32_t participantId = 0;
  bool operator==(const PreviewSlot& o) const {
    return slot == o.slot && sourceUuid == o.sourceUuid && participantId == o.participantId;
  }
};

struct PreviewMeasure {
  std::uint32_t sourceWidth = 0, sourceHeight = 0;
  std::uint64_t frames = 0;
  int fpsX100 = -1;           // -1 until a full second has been measured
  bool hasAudio = false;
  int audioPeakDbX100 = 0;    // valid when hasAudio
};

class PreviewPublisher {
 public:
  static constexpr int kWidth = 480;
  static constexpr int kHeight = 270;

  PreviewPublisher();
  ~PreviewPublisher();
  PreviewPublisher(const PreviewPublisher&) = delete;
  PreviewPublisher& operator=(const PreviewPublisher&) = delete;

  // Creates (and empties) `dir`. Returns false with `error` when it cannot.
  bool start(const std::filesystem::path& dir, std::string& error);
  void stop();

  // The routed slots and the engine instance's SHM prefix. Cheap; call often.
  void setSlots(const std::string& shmPrefix, std::vector<PreviewSlot> slots);

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
  // Measurements by slot (only slots that are routed).
  [[nodiscard]] std::map<int, PreviewMeasure> measures() const;

  // Default location: per-user app data, one folder per node process.
  static std::filesystem::path defaultDir();

  // Pure: I420 (full range BT.709) -> BGRA, letterboxed into dstW x dstH.
  static void i420ToBgraFit(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, int srcW, int srcH,
                            std::uint8_t* bgra, int dstW, int dstH);

 private:
  struct Slot;
  void run();
  void publishSlot(Slot& slot, std::int64_t nowMs);

  std::filesystem::path dir_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  std::string shmPrefix_;
  std::vector<PreviewSlot> wanted_;
  std::map<int, std::unique_ptr<Slot>> slots_;  // owned by the thread
  std::map<int, PreviewMeasure> measures_;      // guarded by mutex_
};

}  // namespace corevideo::edge
