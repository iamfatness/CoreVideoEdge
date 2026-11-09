#pragma once

#include <chrono>
#include <cstdint>

namespace corevideo::edge {

// The engine stamps every ISO arrival with steady_clock in 100 ns ticks
// (zoom-engine/engine/engine-video.cpp, captured100ns). On Windows MSVC's
// steady_clock is QueryPerformanceCounter, which is one system-wide clock, so
// the engine process and this process read the same timeline. The take epoch
// MUST come from this same clock.
inline std::int64_t steadyNow100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}

}  // namespace corevideo::edge
