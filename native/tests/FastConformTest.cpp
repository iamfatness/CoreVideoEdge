#include "TestHarness.h"

#include "edge/FastConform.h"

#include <chrono>
#include <cstdio>
#include <vector>

using corevideo::edge::FastConform;

namespace {
std::vector<std::uint8_t> i420(int w, int h, std::uint8_t y, std::uint8_t u, std::uint8_t v) {
  std::vector<std::uint8_t> f(static_cast<size_t>(w) * h * 3 / 2);
  std::fill(f.begin(), f.begin() + static_cast<long long>(w) * h, y);
  std::fill(f.begin() + static_cast<long long>(w) * h, f.begin() + static_cast<long long>(w) * h * 5 / 4, u);
  std::fill(f.begin() + static_cast<long long>(w) * h * 5 / 4, f.end(), v);
  return f;
}
}  // namespace

TEST(FastConform, SameSizeIsABitExactCopy) {
  auto src = i420(1920, 1080, 0, 0, 0);
  for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i * 7);
  FastConform c;
  std::vector<std::uint8_t> out;
  c.run(src.data(), 1920, 1080, 1920, 1080, out);
  const size_t luma = 1920 * 1080;
  EXPECT_TRUE(std::equal(src.begin(), src.begin() + luma, out.begin()));
  EXPECT_EQ(out[luma], src[luma]);              // U0
  EXPECT_EQ(out[luma + 1], src[luma * 5 / 4]);  // V0
}

TEST(FastConform, A720pGuestFillsThe1080pFrameWithTheSameColour) {
  const auto src = i420(1280, 720, 77, 90, 200);
  FastConform c;
  std::vector<std::uint8_t> out;
  c.run(src.data(), 1280, 720, 1920, 1080, out);
  const size_t luma = 1920 * 1080;
  bool flat = true;
  for (size_t i = 0; i < luma; ++i) flat = flat && out[i] == 77;
  for (size_t i = luma; i < out.size(); i += 2) flat = flat && out[i] == 90 && out[i + 1] == 200;
  EXPECT_TRUE(flat);
}

TEST(FastConform, UpscaledGradientStaysInOrderAndInRange) {
  auto src = i420(1280, 720, 0, 128, 128);
  for (int y = 0; y < 720; ++y)
    for (int x = 0; x < 1280; ++x) src[static_cast<size_t>(y) * 1280 + x] = static_cast<std::uint8_t>(x * 255 / 1279);
  FastConform c;
  std::vector<std::uint8_t> out;
  c.run(src.data(), 1280, 720, 1920, 1080, out);
  bool ordered = true;
  for (int x = 1; x < 1920; ++x) ordered = ordered && out[540 * 1920 + x] >= out[540 * 1920 + x - 1];
  EXPECT_TRUE(ordered);
  EXPECT_TRUE(out[540 * 1920] <= 1);
  EXPECT_TRUE(out[540 * 1920 + 1919] >= 254);
}

TEST(FastConform, A43GuestIsPillarboxedWithFullRangeBlack) {
  const auto src = i420(640, 480, 200, 60, 70);
  FastConform c;
  std::vector<std::uint8_t> out;
  c.run(src.data(), 640, 480, 1920, 1080, out);
  // 4:3 at 1080 high = 1440 wide, bars of 240 each side.
  EXPECT_EQ(out[540 * 1920 + 100], std::uint8_t{0});      // bar: full-range black, not 16
  EXPECT_EQ(out[540 * 1920 + 960], std::uint8_t{200});    // picture
  EXPECT_EQ(out[540 * 1920 + 1919 - 100], std::uint8_t{0});
  const size_t luma = 1920 * 1080;
  EXPECT_EQ(out[luma + 270 * 1920 + 100], std::uint8_t{128});  // bar chroma neutral
}

TEST(FastConform, A720pFrameConformsWellInsideAFrameTime) {
  auto src = i420(1280, 720, 0, 128, 128);
  for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
  FastConform c;
  std::vector<std::uint8_t> out;
  c.run(src.data(), 1280, 720, 1920, 1080, out);  // warm tables
  const int runs = 30;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < runs; ++i) c.run(src.data(), 1280, 720, 1920, 1080, out);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
  std::fprintf(stderr, "  720p -> 1080p: %.2f ms per frame\n", ms);
  // Live budget: 8 ISOs x 30 fps on separate threads; Pro's conform took ~35 ms.
  EXPECT_TRUE(ms < 8.0);
}
