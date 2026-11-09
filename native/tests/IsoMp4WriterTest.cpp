#include "TestHarness.h"

#include "edge/IsoMp4Writer.h"

#include <mfapi.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace corevideo::edge;

namespace {

struct MfScope {
  MfScope() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
  }
  ~MfScope() {
    MFShutdown();
    CoUninitialize();
  }
};

std::string run(const std::string& command) {
  std::string out;
  if (FILE* pipe = _popen(command.c_str(), "r")) {
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    _pclose(pipe);
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
  if (probe.empty() || probe.find("not recognized") != std::string::npos) {
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
}

TEST(IsoMp4Writer, AnUnwritablePathFailsLoudly) {
  MfScope mf;
  IsoMp4Writer writer;
  std::string error;
  EXPECT_FALSE(writer.open("Z:\\no\\such\\volume\\x.mp4", IsoWriterSettings{}, error));
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
  if (FILE* pipe = _popen(("ffmpeg -v error -i \"" + path.string() + "\" -map 0:a:0 -ac 1 -f s16le -").c_str(), "rb")) {
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) pcm.append(buffer, n);
    _pclose(pipe);
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
  EXPECT_TRUE(writer.videoDropped() > 0);  // the muxer really does hold video for audio
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
