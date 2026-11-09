#include "TestHarness.h"

#include "edge/IsoSession.h"
#include "modules/IsoFrameConform.h"

using namespace corevideo::edge;

TEST(IsoNaming, IsoFileNamesUseTheRosterName) {
  EXPECT_EQ(isoFileName(1, "Dr. Jane O'Neil", "zoom101"), std::string("ISO-01-Dr-Jane-ONeil.mp4"));
  EXPECT_EQ(isoFileName(12, "../../evil\\name", "zoom7"), std::string("ISO-12-evilname.mp4"));
  EXPECT_EQ(isoFileName(3, "", "zoom555"), std::string("ISO-03-zoom555.mp4"));
}

TEST(IsoNaming, SessionFolderIsPrefixAndStamp) {
  EXPECT_EQ(sessionFolderName("Edge 01", "20261108-214100"), std::string("Edge-01-20261108-214100"));
  EXPECT_EQ(sessionFolderName("", "20261108-214100"), std::string("edge-20261108-214100"));
}

TEST(IsoDisk, RefusesWhenTenMinutesDoNotFit) {
  IsoWriterSettings writer;  // 18 Mbps + 192 kbps per ISO
  const auto tight = estimateDisk(3ull * 1000 * 1000 * 1000, 6, writer);
  EXPECT_FALSE(tight.refusal.empty());
  const auto roomy = estimateDisk(500ull * 1000 * 1000 * 1000, 6, writer);
  EXPECT_TRUE(roomy.refusal.empty());
  EXPECT_TRUE(roomy.warning.empty());
}

TEST(IsoDisk, WarnsUnderAnHour) {
  IsoWriterSettings writer;
  // 6 ISOs ~ 13.6 MB/s; 30 GB is ~36 minutes.
  const auto d = estimateDisk(30ull * 1000 * 1000 * 1000, 6, writer);
  EXPECT_TRUE(d.refusal.empty());
  EXPECT_FALSE(d.warning.empty());
}

TEST(IsoConform, A360pRampFrameIsScaledIntoTheFullSizeFile) {
  // 640x360 grey frame into the 1920x1080 writer: no bars (same aspect), all
  // luma carried through.
  std::vector<std::uint8_t> i420(640 * 360 * 3 / 2, 128);
  std::fill(i420.begin(), i420.begin() + 640 * 360, 200);
  std::vector<std::uint8_t> nv12;
  corevideo::modules::conformI420ToNv12(i420.data(), 640, 360, 1920, 1080, nv12);
  EXPECT_EQ(nv12.size(), size_t{1920 * 1080 * 3 / 2});
  EXPECT_EQ(static_cast<int>(nv12[0]), 200);
  EXPECT_EQ(static_cast<int>(nv12[1920 * 1080 - 1]), 200);
}
