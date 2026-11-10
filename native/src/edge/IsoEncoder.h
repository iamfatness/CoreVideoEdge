#pragma once

// The platform half of IsoMp4Writer: one H.264 + AAC MP4 file.
//
// IsoMp4Writer (portable) owns every timing rule: the held-frame VFR / constant
// grid, preroll, repeats, the backlog cap, sample-counted audio and the common
// end. A backend only encodes and muxes what it is handed, with the timestamps
// it is handed:
//  - Windows: Media Foundation Sink Writer + fragmented MP4 sink
//    (IsoEncoderMediaFoundation.cpp; Pro's Mp4Writer rules);
//  - macOS: VideoToolbox H.264 + AVAssetWriter (AAC from LPCM), fragmented
//    (IsoEncoderAVFoundation.mm).
//
// Single-threaded from the caller's side: every call comes from the owning
// recorder thread.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace corevideo::edge {

struct IsoWriterSettings {
  int width = 1920;
  int height = 1080;
  int nominalFps = 30;
  int videoBitrateMbps = 18;   // Pro's ISO default
  int audioBitrateKbps = 192;
  int audioSampleRate = 48000;
  bool hardwareEncode = true;
  int maxVideoBacklog = 60;    // samples (2 s at 30 fps, ~190 MB at 1080p NV12)
  int constantFps = 0;         // 0: variable, as Zoom delivers. 30 | 60: every sample on a fixed grid
};

class IsoEncoderBackend {
 public:
  // One uncompressed NV12 picture owned by the backend's native buffer type
  // (IMFMediaBuffer / CVPixelBuffer). A held frame is re-submitted by handle,
  // never copied again.
  class Frame {
   public:
    virtual ~Frame() = default;
  };
  using FramePtr = std::shared_ptr<Frame>;

  virtual ~IsoEncoderBackend() = default;

  // Creates the file with an H.264 video stream and an AAC stereo stream and
  // begins writing. `settings` are already normalized by IsoMp4Writer.
  virtual bool open(const std::filesystem::path& path, const IsoWriterSettings& settings, std::string& error) = 0;
  // NV12 at exactly width x height; nullptr makes a full-range black frame.
  virtual FramePtr makeFrame(const std::uint8_t* nv12, std::string& error) = 0;
  // One video sample: shown from pts100ns for duration100ns. Strictly
  // increasing pts.
  virtual bool writeVideo(const FramePtr& frame, std::int64_t pts100ns, std::int64_t duration100ns,
                          std::string& error) = 0;
  // Interleaved stereo int16 at sample position `startSample` (nullptr =
  // silence). Positions are contiguous.
  virtual bool writeAudio(const std::int16_t* pcm, std::int64_t startSample, std::int64_t frames,
                          std::string& error) = 0;
  // Video samples handed to the backend that have not reached the file yet.
  [[nodiscard]] virtual std::int64_t videoBacklog() const = 0;
  // Ends both streams at `end100ns` and closes the container (bounded wait).
  virtual bool finalize(std::int64_t end100ns, std::string& error) = 0;
  // Abandons the file (open failure / destructor without finalize).
  virtual void reset() = 0;
  // Measured, not requested: "hardware" | "software" | "" (unknown).
  [[nodiscard]] virtual std::string encoderPath() const = 0;
};

std::unique_ptr<IsoEncoderBackend> makeIsoEncoderBackend();

// Per-thread platform media setup the recorder thread needs before open()
// (Windows: COM MTA + MFStartup; macOS: nothing). `error` set when false.
class IsoMediaThreadScope {
 public:
  IsoMediaThreadScope();
  ~IsoMediaThreadScope();
  IsoMediaThreadScope(const IsoMediaThreadScope&) = delete;
  IsoMediaThreadScope& operator=(const IsoMediaThreadScope&) = delete;
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  bool ok_ = false;
  bool com_ = false;
  bool mf_ = false;
  std::string error_;
};

}  // namespace corevideo::edge
