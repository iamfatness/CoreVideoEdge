#pragma once

// One live feed of one routed slot to CoreVideo Pro (NDI or SRT).

#include <cstdint>
#include <string>

namespace corevideo::edge {

class FeedSink {
 public:
  virtual ~FeedSink() = default;
  virtual bool open(std::string& error) = 0;
  // Full-range BT.709 I420 at the size Zoom delivered.
  virtual void video(const std::uint8_t* i420, int w, int h) = 0;
  // Interleaved stereo float, 48 kHz, continuous (silence already filled).
  virtual void audio(const float* stereo, int frames) = 0;
  virtual int receivers() { return -1; }       // measured receivers, -1 = this transport cannot tell
  virtual std::string problem() { return {}; } // non-empty: failing, and why (operator-readable)
  virtual bool connected() { return false; }   // SRT: the caller reached Pro's listener
  virtual void setGuest(const std::string&, std::uint32_t) {}
};

}  // namespace corevideo::edge
