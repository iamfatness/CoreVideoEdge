#include "TestHarness.h"

#include "edge/IsoMp4Writer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace corevideo::edge;

namespace {

// Windows: COM + Media Foundation for this thread. macOS: nothing.
using MfScope = IsoMediaThreadScope;

#if defined(_WIN32)
FILE* openPipe(const std::string& command, bool binary) { return _popen(command.c_str(), binary ? "rb" : "r"); }
void closePipe(FILE* pipe) { _pclose(pipe); }
#else
FILE* openPipe(const std::string& command, bool) { return popen(command.c_str(), "r"); }
void closePipe(FILE* pipe) { pclose(pipe); }
#endif

// The probe tool is missing (Windows cmd / POSIX sh wording).
bool toolMissing(const std::string& out) {
  return out.empty() || out.find("not recognized") != std::string::npos || out.find("not found") != std::string::npos;
}

std::string run(const std::string& command) {
  std::string out;
  if (FILE* pipe = openPipe(command, false)) {
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    closePipe(pipe);
  }
  return out;
}

std::filesystem::path testDir() {
  // Test scratch only. The product never writes to %TEMP%.
  auto dir = std::filesystem::temp_directory_path() / "corevideo-edge-tests";
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

TEST(IsoMp4Writer, WritesAPlayableVideoPlusAudioFile) {
  MfScope mf;
  const auto path = testDir() / "writer-test.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;  // deterministic on any CI box
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  EXPECT_TRUE(error.empty());

  // 2 s of 30 fps video starting 0.5 s after the epoch, with a 0.5 s camera
  // stall in the middle; 1 s of tone, a 1 s pause, 0.5 s of tone.
  std::vector<std::uint8_t> nv12(1920 * 1080 * 3 / 2, 128);
  int written = 0;
  for (int i = 0; i < 60; ++i) {
    if (i >= 30 && i < 45) continue;  // stall
    std::fill(nv12.begin(), nv12.begin() + 1920 * 1080, static_cast<std::uint8_t>(16 + i * 3));
    ASSERT_TRUE(writer.writeVideo(nv12.data(), 5'000'000 + static_cast<std::int64_t>(i) * 333'333, error));
    ++written;
  }
  std::vector<float> tone(48000 * 2);
  for (int i = 0; i < 48000; ++i) tone[i * 2] = tone[i * 2 + 1] = static_cast<float>(0.25 * std::sin(i * 2 * 3.14159265 * 440 / 48000));
  ASSERT_TRUE(writer.writeAudio(tone.data(), 48000, error));
  ASSERT_TRUE(writer.writeSilence(48000, error));
  ASSERT_TRUE(writer.writeAudio(tone.data(), 24000, error));
  EXPECT_TRUE(writer.finalize(30'000'000, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(writer.videoFrames(), static_cast<std::int64_t>(written));
  EXPECT_EQ(writer.prerollFrames(), std::int64_t{1});
  EXPECT_TRUE(std::filesystem::file_size(path) > 10000);

  const auto probe = run("ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,sample_rate,channels "
                         "-show_entries format=duration -of compact \"" + path.string() + "\" 2>&1");
  if (toolMissing(probe)) {
    std::fprintf(stderr, "  (ffprobe not on PATH; container check skipped)\n");
    return;
  }
  EXPECT_TRUE(probe.find("codec_type=video") != std::string::npos);
  EXPECT_TRUE(probe.find("codec_name=h264") != std::string::npos);
  EXPECT_TRUE(probe.find("width=1920|height=1080") != std::string::npos);
  EXPECT_TRUE(probe.find("codec_type=audio") != std::string::npos);
  EXPECT_TRUE(probe.find("codec_name=aac") != std::string::npos);
  EXPECT_TRUE(probe.find("sample_rate=48000") != std::string::npos);
  const auto at = probe.find("duration=");
  ASSERT_TRUE(at != std::string::npos);
  const double duration = std::atof(probe.c_str() + at + 9);
  // Both tracks were brought to the 3.0 s take end.
  EXPECT_TRUE(duration > 2.9 && duration < 3.2);
  if (!(duration > 2.9 && duration < 3.2)) std::fprintf(stderr, "  ffprobe: %s\n", probe.c_str());
  // Informational: what the platform encoder declared (full-range BT.709 is the intent).
  std::fprintf(stderr, "  encoder=%s %s", writer.encoderPath().c_str(),
               run("ffprobe -v error -select_streams v -show_entries stream=color_range,color_space,color_primaries,"
                   "color_transfer,profile -of compact \"" + path.string() + "\" 2>&1").c_str());
}

TEST(IsoMp4Writer, AnUnwritablePathFailsLoudly) {
  MfScope mf;
  IsoMp4Writer writer;
  std::string error;
#if defined(_WIN32)
  EXPECT_FALSE(writer.open("Z:\\no\\such\\volume\\x.mp4", IsoWriterSettings{}, error));
#else
  EXPECT_FALSE(writer.open("/no/such/volume/x.mp4", IsoWriterSettings{}, error));
#endif
  EXPECT_FALSE(error.empty());
}

TEST(IsoMp4Writer, AudioDecodesAtTheSamplePositionItWasWritten) {
  // Guards the silence-fill timeline: if the AAC path ever adds encoder delay
  // (priming without an edit list), every ISO stem would land late by it.
  // Measured 2026-11-08: 0 samples on the Windows AAC MFT + ffmpeg decode.
  MfScope mf;
  const auto path = testDir() / "aac-position.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  std::vector<std::uint8_t> nv12(1920 * 1080 * 3 / 2, 128);
  writer.writeVideo(nv12.data(), 10'000'000, error);
  ASSERT_TRUE(writer.writeSilence(48000, error));
  std::vector<float> burst(96 * 2);
  for (int i = 0; i < 96; ++i) burst[i * 2] = burst[i * 2 + 1] = ((i / 12) & 1) ? -0.97f : 0.97f;
  ASSERT_TRUE(writer.writeAudio(burst.data(), 96, error));
  ASSERT_TRUE(writer.writeSilence(48000, error));
  ASSERT_TRUE(writer.finalize(0, error));
  std::string pcm;
  if (FILE* pipe = openPipe("ffmpeg -v error -i \"" + path.string() + "\" -map 0:a:0 -ac 1 -f s16le -", true)) {
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) pcm.append(buffer, n);
    closePipe(pipe);
  }
  if (pcm.empty()) {
    std::fprintf(stderr, "  (ffmpeg not on PATH; decode check skipped)\n");
    return;
  }
  const auto* x = reinterpret_cast<const std::int16_t*>(pcm.data());
  long long found = -1;
  for (size_t i = 0; i < pcm.size() / 2; ++i) {
    if (x[i] > 16000 || x[i] < -16000) { found = static_cast<long long>(i); break; }
  }
  ASSERT_TRUE(found >= 0);
  EXPECT_TRUE(found >= 48000 - 48 && found <= 48000 + 48);  // within 1 ms
  if (!(found >= 48000 - 48 && found <= 48000 + 48)) std::fprintf(stderr, "  burst decoded at sample %lld\n", found);
}

// Live 2026-11-09: with no audio advancing, the MP4 muxer held every video
// frame and the node reached 50 GB. The writer must never queue past its cap.
TEST(IsoMp4Writer, VideoWithAStalledAudioTrackIsBoundedNotBuffered) {
  MfScope mf;
  const auto path = testDir() / "writer-backlog.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;
  settings.maxVideoBacklog = 30;
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  std::vector<std::uint8_t> nv12(1920 * 1080 * 3 / 2, 128);
  for (int i = 0; i < 300; ++i)  // 10 s of video, no audio at all
    ASSERT_TRUE(writer.writeVideo(nv12.data(), 1'000 + static_cast<std::int64_t>(i) * 333'333, error));
  std::fprintf(stderr, "  backlogPeak=%lld dropped=%lld frames=%lld\n",
               static_cast<long long>(writer.videoBacklogPeak()), static_cast<long long>(writer.videoDropped()),
               static_cast<long long>(writer.videoFrames()));
  EXPECT_TRUE(writer.videoBacklogPeak() <= settings.maxVideoBacklog + 2);
#if defined(_WIN32)
  EXPECT_TRUE(writer.videoDropped() > 0);  // the MF muxer really does hold video for audio
#else
  // AVAssetWriter (realtime inputs) does not hold video for a silent audio
  // track: measured on the macOS runner 2026-11-09, backlogPeak 9, 0 dropped.
  // The cap above is the guarantee that matters on both platforms.
#endif
  EXPECT_TRUE(writer.finalize(100'000'000, error));
}

TEST(IsoMp4Writer, RepeatingTheHeldFrameKeepsAStalledCameraMoving) {
  MfScope mf;
  const auto path = testDir() / "writer-held.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  // Camera never sends a frame; 10 s of audio arrives; the held (preroll)
  // frame is repeated once a second.
  std::vector<float> tone(4800 * 2, 0.1f);
  for (int s = 1; s <= 10; ++s) {
    for (int k = 0; k < 10; ++k) ASSERT_TRUE(writer.writeAudio(tone.data(), 4800, error));
    ASSERT_TRUE(writer.repeatHeldVideo(static_cast<std::int64_t>(s) * 10'000'000, error));
  }
  EXPECT_TRUE(writer.repeatHeldVideo(5 * 10'000'000, error));  // not after the held frame: no-op
  EXPECT_EQ(writer.heldRepeats(), std::int64_t{9});  // the 10th is still held when finalize flushes it
  EXPECT_EQ(writer.videoFrames(), std::int64_t{0});
  EXPECT_TRUE(writer.finalize(100'000'000, error));
  EXPECT_EQ(writer.heldRepeats(), std::int64_t{10});
}

// Record format: 30 fps constant from an irregular ~46 fps source (what live
// Zoom delivered 2026-11-09). Every sample sits on the 1/30 s grid.
TEST(IsoMp4Writer, ConstantThirtyFromAnIrregularSourceIsOnTheGrid) {
  MfScope mf;
  const auto path = testDir() / "writer-cfr30.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;
  settings.constantFps = 30;
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  std::vector<std::uint8_t> nv12(1920 * 1080 * 3 / 2, 128);
  std::int64_t pts = 0;
  const int gaps[] = {217'391, 150'000, 260'000, 217'391, 90'000};  // ~46 fps, jittery
  for (int i = 0; pts < 30'000'000; ++i) {  // 3 s
    pts += gaps[i % 5];
    ASSERT_TRUE(writer.writeVideo(nv12.data(), pts, error));
  }
  std::vector<float> tone(48000 * 3 * 2, 0.0f);
  ASSERT_TRUE(writer.writeAudio(tone.data(), 48000 * 3, error));
  // A camera stall of 1 s: the grid is filled while waiting.
  ASSERT_TRUE(writer.repeatHeldVideo(40'000'000, error));
  EXPECT_TRUE(writer.finalize(40'000'000, error));
  std::fprintf(stderr, "  frames=%lld cadenceRepeats=%lld cadenceSkipped=%lld held=%lld\n",
               static_cast<long long>(writer.videoFrames()), static_cast<long long>(writer.cadenceRepeats()),
               static_cast<long long>(writer.cadenceSkipped()), static_cast<long long>(writer.heldRepeats()));
  EXPECT_TRUE(writer.cadenceSkipped() > 0);  // 46 into 30: some arrivals share a slot
  // The 1 s stall is filled either as stall repeats or, if the encoder was
  // briefly backed up, by the final grid fill: either way the grid is complete.
  // 120, or 121 when the stall fill reached the stop slot itself (an encoder
  // that never refuses the fill, as on macOS): the take then ends one frame
  // after the stop, by the common-end rule.
  const auto samples = writer.videoFrames() + writer.cadenceRepeats() + writer.heldRepeats() + writer.prerollFrames();
  EXPECT_TRUE(samples == 120 || samples == 121);
  // 4 s at 30 fps = 120 samples (+-1 at the edges), all of them on the grid.
  const auto out = run("ffprobe -v error -select_streams v -count_packets -show_entries stream=nb_read_packets,r_frame_rate,avg_frame_rate "
                       "-of compact \"" + path.string() + "\" 2>&1");
  if (toolMissing(out)) return;
  std::fprintf(stderr, "  ffprobe: %s", out.c_str());
  const auto at = out.find("nb_read_packets=");
  ASSERT_TRUE(at != std::string::npos);
  const int packets = std::atoi(out.c_str() + at + 16);
  EXPECT_TRUE(packets >= 119 && packets <= 121);
  EXPECT_TRUE(out.find("avg_frame_rate=30/1") != std::string::npos || out.find("r_frame_rate=30/1") != std::string::npos);
}

TEST(IsoMp4Writer, A720pRecordFormatWritesA720pFile) {
  MfScope mf;
  const auto path = testDir() / "writer-720.mp4";
  IsoMp4Writer writer;
  IsoWriterSettings settings;
  settings.hardwareEncode = false;
  settings.width = 1280;
  settings.height = 720;
  std::string error;
  ASSERT_TRUE(writer.open(path, settings, error));
  std::vector<std::uint8_t> nv12(1280 * 720 * 3 / 2, 100);
  for (int i = 0; i < 30; ++i) ASSERT_TRUE(writer.writeVideo(nv12.data(), 333'333LL * i + 1, error));
  std::vector<float> tone(48000 * 2, 0.0f);
  ASSERT_TRUE(writer.writeAudio(tone.data(), 48000, error));
  EXPECT_TRUE(writer.finalize(10'000'000, error));
  const auto out = run("ffprobe -v error -select_streams v -show_entries stream=width,height -of compact \"" + path.string() + "\" 2>&1");
  if (toolMissing(out)) return;
  EXPECT_TRUE(out.find("width=1280|height=720") != std::string::npos);
}
