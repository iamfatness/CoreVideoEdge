#pragma once

// Readers of the engine's shared-memory regions (preview, feeds, ISO) and the
// one way POSIX differs from Windows.
//
// Windows: a named section lives while ANY handle is open. When the engine
// destroys a region (unsubscribe, Engine Off, room move) and later creates it
// again under the same name, it gets the SAME section the node still maps, so
// the node keeps reading live data without doing anything.
//
// POSIX (macOS): shm_unlink + shm_open(O_CREAT) makes a NEW object under the
// name (engine-ipc.h unlinks on every create: an object can be sized once).
// The node's mapping keeps the orphan alive and reads it forever: a frozen
// preview, a feed of one still frame, an ISO that stops after a room move.
//
// So on POSIX a reader closes and reopens a region whose progress counter
// (frame sequence / ring write counter) has not moved for kShmReopenAfterMs.
// A live region always moves; an idle one costs one shm_open + mmap per
// interval. On Windows this never fires.

#include <chrono>
#include <cstdint>
#include <limits>

namespace corevideo::edge {

inline constexpr std::int64_t kShmReopenAfterMs = 250;

struct ShmWatch {
  std::uint64_t lastProgress = std::numeric_limits<std::uint64_t>::max();
  std::int64_t sinceMs = 0;

  // `progress`: the region's monotonic counter right now. True when the
  // caller should shm_region_destroy() the region so its normal open path
  // maps the current object.
  bool shouldReopen(std::uint64_t progress, std::int64_t nowMs) {
#if defined(_WIN32)
    (void)progress;
    (void)nowMs;
    return false;
#else
    if (progress != lastProgress) {
      lastProgress = progress;
      sinceMs = nowMs;
      return false;
    }
    if (nowMs - sinceMs < kShmReopenAfterMs) return false;
    sinceMs = nowMs;
    return true;
#endif
  }
};

inline std::int64_t shmWatchNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace corevideo::edge
