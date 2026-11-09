#pragma once

// Live feeds of every routed slot to CoreVideo Pro (spec:
// docs/superpowers/specs/2026-11-09-edge-to-pro-ndi-srt-design.md).
//
// One thread per routed slot reads the engine's main video region (latest
// frame, sequence-checked) and the guest's isolated audio ring (own cursor),
// fills audio silence in real time while the guest is quiet, and hands both to
// a sink (NDI today; SRT next). Health is measured: frames sent, frames the
// engine delivered that were not sent in time, receivers connected.

#include "edge/FeedTargets.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::edge {

struct FeedSlotSpec {
  int slot = 0;
  std::string sourceUuid;
  std::uint32_t participantId = 0;
  std::string guestName;
};

struct FeedHealth {
  int slot = 0;
  std::string target;          // NDI name or redacted SRT URL
  std::string state;           // starting | sending | waiting | failed
  std::uint64_t framesSent = 0;
  std::uint64_t framesSkipped = 0;  // delivered by Zoom but superseded before they could be sent
  std::uint64_t audioSeconds = 0;
  int receivers = -1;          // NDI: connected receivers (-1 = not reported)
  bool connected = false;      // SRT: reached Pro's listener
  std::string error;
};

class FeedSender {
 public:
  FeedSender();
  ~FeedSender();
  FeedSender(const FeedSender&) = delete;
  FeedSender& operator=(const FeedSender&) = delete;

  // Mode and SRT settings. Empty return = accepted.
  std::string configure(FeedMode mode, const SrtSettings& srt, const std::string& nodeName);
  void setSlots(const std::string& shmPrefix, std::vector<FeedSlotSpec> slots);
  void stopAll();

  [[nodiscard]] FeedMode mode() const;
  [[nodiscard]] SrtSettings srt() const;
  [[nodiscard]] std::vector<FeedHealth> health() const;
  [[nodiscard]] std::string modeError() const;  // e.g. NDI runtime missing

 private:
  struct Worker;
  void reconcile();

  mutable std::mutex mutex_;
  FeedMode mode_ = FeedMode::Off;
  SrtSettings srt_;
  std::string nodeName_;
  std::string shmPrefix_;
  std::string modeError_;
  std::vector<FeedSlotSpec> wanted_;
  std::map<int, std::unique_ptr<Worker>> workers_;
};

}  // namespace corevideo::edge
