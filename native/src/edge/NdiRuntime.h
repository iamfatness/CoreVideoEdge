#pragma once

// The NDI runtime, loaded at run time (never linked, never committed): the
// same v5 C ABI subset and the same runtime search list CoreVideo Pro uses
// (NdiOutputSenderAdapter.cpp / NdiReceiveCaptureAdapter.cpp), plus the macOS
// library name. Send for Edge's feeds; find/receive for the verification probe.

#include <cstdint>
#include <string>

namespace corevideo::edge::ndi {

extern "C" {
struct Source { const char* name; const char* url; };
struct FindCreate { bool showLocal; const char* groups; const char* extraIps; };
struct RecvCreate {
  Source source;
  int colorFormat;   // 0 = BGRX/BGRA, 1 = UYVY/BGRA, 100 = fastest
  int bandwidth;     // 100 = highest
  bool allowFields;
  const char* receiverName;
};
struct SendCreate { const char* name; const char* groups; bool clockVideo; bool clockAudio; };
struct Video {
  int width, height, fourcc, fpsNumerator, fpsDenominator;
  float aspect;
  int frameFormat;  // 1 = progressive
  std::int64_t timecode;
  const std::uint8_t* data;
  int stride;
  const char* metadata;
  std::int64_t timestamp;
};
struct Audio {
  int sampleRate, channels, samples;
  std::int64_t timecode;
  const float* data;  // planar
  int channelStrideBytes;
  const char* metadata;
  std::int64_t timestamp;
};
}  // extern "C"

static_assert(sizeof(Source) == 16 && sizeof(RecvCreate) == 40);
static_assert(sizeof(Video) == 72 && sizeof(Audio) == 56);

inline constexpr int kFourccI420 = 0x30323449;  // 'I420'
inline constexpr int kFourccUyvy = 0x59565955;
inline constexpr int kFourccBgra = 0x41524742;
inline constexpr int kFourccBgrx = 0x58524742;
inline constexpr std::int64_t kTimecodeSynthesize = INT64_MAX;
enum FrameType { kNone = 0, kVideo = 1, kAudio = 2, kMetadata = 3, kError = 4, kStatusChange = 100 };

struct Api {
  bool (*initialize)() = nullptr;
  void* (*sendCreate)(const SendCreate*) = nullptr;
  void (*sendDestroy)(void*) = nullptr;
  void (*sendVideo)(void*, const Video*) = nullptr;
  void (*sendAudio)(void*, const Audio*) = nullptr;
  int (*sendConnections)(void*, std::uint32_t) = nullptr;
  void* (*findCreate)(const FindCreate*) = nullptr;
  void (*findDestroy)(void*) = nullptr;
  bool (*findWait)(void*, std::uint32_t) = nullptr;
  const Source* (*findSources)(void*, std::uint32_t*) = nullptr;
  void* (*recvCreate)(const RecvCreate*) = nullptr;
  void (*recvDestroy)(void*) = nullptr;
  int (*recvCapture)(void*, Video*, Audio*, void*, std::uint32_t) = nullptr;
  void (*freeVideo)(void*, const Video*) = nullptr;
  void (*freeAudio)(void*, const Audio*) = nullptr;
  [[nodiscard]] bool canSend() const { return initialize && sendCreate && sendDestroy && sendVideo && sendAudio; }
  [[nodiscard]] bool canReceive() const {
    return initialize && findCreate && findDestroy && findWait && findSources && recvCreate && recvDestroy &&
           recvCapture && freeVideo && freeAudio;
  }
};

// Loads once per process. nullptr when the runtime is not installed; `error`
// then names what was tried (operator-readable).
const Api* load(std::string& error);

}  // namespace corevideo::edge::ndi
