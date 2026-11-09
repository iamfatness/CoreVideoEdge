#pragma once

// SRT feed of one routed slot into CoreVideo Pro's SRT input listener, via an
// FFmpeg child process (Pro's own SRT stack is FFmpeg).
//
//   video: conformed to 1920x1080 NV12 (the stream never changes size while
//          Zoom ramps 360p -> 1080p), paced at a constant 30 fps on stdin;
//   audio: f32le 48 kHz stereo on a named pipe, continuous;
//   out:   H.264 (h264_nvenc low-latency, libx264 zerolatency fallback) + AAC
//          in MPEG-TS, srt://<pro>:<basePort+slot-1>?mode=caller.
//
// Health is measured from the process: alive, the SRT connection reached
// (FFmpeg starts writing output), or the reason it failed (from its log). A
// failed connection retries every 3 s, stated in health. Windows only in this
// build; the POSIX route (pipe:3, as Pro does) is the Mac follow-up.

#include "edge/FeedSink.h"
#include "edge/FeedTargets.h"

#include <memory>

namespace corevideo::edge {

std::unique_ptr<FeedSink> makeSrtFeedSink(const SrtSettings& settings, int slot);

// Locates ffmpeg: COREVIDEO_FFMPEG_DIR, then PATH. Empty when not found.
std::string findFfmpeg();

}  // namespace corevideo::edge
