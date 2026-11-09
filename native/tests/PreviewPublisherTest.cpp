#include "TestHarness.h"

#include "edge/PreviewPublisher.h"

#include <vector>

using corevideo::edge::PreviewPublisher;

namespace {
struct Rgb { int r, g, b; };
Rgb convert(int y, int u, int v) {
  std::vector<std::uint8_t> Y(4 * 4, static_cast<std::uint8_t>(y)), U(2 * 2, static_cast<std::uint8_t>(u)),
      V(2 * 2, static_cast<std::uint8_t>(v));
  std::vector<std::uint8_t> out(16 * 9 * 4);
  PreviewPublisher::i420ToBgraFit(Y.data(), U.data(), V.data(), 4, 4, out.data(), 16, 9);
  const size_t centre = (4 * 16 + 8) * 4;  // row 4, col 8: inside the pillarboxed picture
  return {out[centre + 2], out[centre + 1], out[centre + 0]};
}
bool near(int a, int b) { return a >= b - 3 && a <= b + 3; }
}  // namespace

TEST(PreviewPublisher, FullRangeBt709ColoursConvert) {
  const auto white = convert(255, 128, 128);
  EXPECT_TRUE(near(white.r, 255) && near(white.g, 255) && near(white.b, 255));
  const auto black = convert(0, 128, 128);
  EXPECT_TRUE(near(black.r, 0) && near(black.g, 0) && near(black.b, 0));
  // BT.709 full-range red: Y = 54, Cb = 99, Cr = 255.
  const auto red = convert(54, 99, 255);
  EXPECT_TRUE(red.r >= 250 && red.g <= 6 && red.b <= 6);
}

TEST(PreviewPublisher, A43SourceIsPillarboxedNotStretched) {
  std::vector<std::uint8_t> Y(4 * 3, 255), U(2, 128), V(2, 128);  // 4:3 white
  std::vector<std::uint8_t> out(16 * 9 * 4);
  PreviewPublisher::i420ToBgraFit(Y.data(), U.data(), V.data(), 4, 3, out.data(), 16, 9);
  EXPECT_EQ(out[(4 * 16 + 0) * 4], std::uint8_t{0});    // left bar black
  EXPECT_EQ(out[(4 * 16 + 8) * 4], std::uint8_t{255});  // picture white
  EXPECT_EQ(out[(4 * 16 + 0) * 4 + 3], std::uint8_t{255});  // opaque
}
