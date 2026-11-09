#include "TestHarness.h"

#include "edge/FeedTargets.h"

using namespace corevideo::edge;

TEST(FeedTargets, NdiNamesAreStablePerNodeAndSlot) {
  EXPECT_EQ(ndiFeedName("CoreVideo Edge - EDGE-01", 3), std::string("CoreVideo Edge EDGE-01 S3"));
  EXPECT_EQ(ndiFeedName("Stage Left/2", 1), std::string("CoreVideo Edge Stage Left-2 S1"));
  EXPECT_EQ(ndiFeedName("", 8), std::string("CoreVideo Edge node S8"));
  EXPECT_EQ(ndiFeedName("CoreVideo Edge (validator)", 2), std::string("CoreVideo Edge validator S2"));
}

TEST(FeedTargets, SrtSlotsMapOntoProsListenerPorts) {
  SrtSettings s;
  s.host = "10.0.0.5";
  EXPECT_EQ(srtPort(s, 1), 10000);
  EXPECT_EQ(srtPort(s, 8), 10007);
  EXPECT_EQ(srtUrl(s, 2, false), std::string("srt://10.0.0.5:10001?mode=caller&transtype=live&latency=120000"));
}

TEST(FeedTargets, PassphraseIsValidatedAndNeverShownInState) {
  SrtSettings s;
  s.host = "pro";
  s.passphrase = "short";
  EXPECT_FALSE(validateSrt(s).empty());
  s.passphrase = "correct horse battery";
  EXPECT_TRUE(validateSrt(s).empty());
  const auto shown = srtUrl(s, 1, true);
  EXPECT_TRUE(shown.find("correct") == std::string::npos);
  EXPECT_TRUE(shown.find("<redacted>") != std::string::npos);
  EXPECT_TRUE(srtUrl(s, 1, false).find("passphrase=correct horse battery") != std::string::npos);
}

TEST(FeedTargets, SrtNeedsProsAddress) {
  SrtSettings s;
  EXPECT_FALSE(validateSrt(s).empty());
  FeedMode m;
  EXPECT_TRUE(parseFeedMode("ndi", m) && m == FeedMode::Ndi);
  EXPECT_FALSE(parseFeedMode("rtmp", m));
}
