#include "TestHarness.h"

#include "edge/EdgeConfig.h"

using namespace corevideo::edge;

TEST(EdgeConfig, ReadsEveryField) {
  std::vector<std::string> errors;
  const auto c = parseEdgeConfig(R"({"displayName":"Edge A","recordFolder":"D:\\ISO","filePrefix":"showA",)"
                                 R"("maxSubscriptions":4,"subscribe":["Alice","Bob"],"videoBitrateMbps":20,)"
                                 R"("hardwareEncode":false})", errors);
  EXPECT_TRUE(errors.empty());
  EXPECT_EQ(c.displayName, std::string("Edge A"));
  EXPECT_EQ(c.recordFolder, std::string("D:\\ISO"));
  EXPECT_EQ(c.maxSubscriptions, 4);
  EXPECT_EQ(c.subscribe.size(), size_t{2});
  EXPECT_FALSE(c.hardwareEncode);
}

TEST(EdgeConfig, BadValuesAreErrorsNotSilentDefaults) {
  std::vector<std::string> errors;
  const auto c = parseEdgeConfig(R"({"maxSubscriptions":30,"subscribe":"Alice","hardwareEncode":"yes"})", errors);
  EXPECT_EQ(errors.size(), size_t{3});
  EXPECT_EQ(c.maxSubscriptions, 8);
}

TEST(EdgeConfig, NoRecordFolderIsInvented) {
  std::vector<std::string> errors;
  const auto c = parseEdgeConfig("{}", errors);
  EXPECT_TRUE(c.recordFolder.empty());
  EXPECT_FALSE(c.displayName.empty());
}
