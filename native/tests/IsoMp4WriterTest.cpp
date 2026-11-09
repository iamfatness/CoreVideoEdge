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
