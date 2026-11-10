// ZComms' assertion macros (tests/audio/test_util.h @ 168a572) re-expressed on
// Edge's TestHarness, so the ported ZComms tests compile unchanged. A failed
// ZC_CHECK is an Edge test failure with ZComms' own case name attached.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "TestHarness.h"

namespace zctest {
inline const char*& CurrentTest() {
  static const char* t = "";
  return t;
}
}  // namespace zctest

#define ZC_CHECK(cond)                                                                        \
  do {                                                                                        \
    if (!(cond))                                                                              \
      edge_test::fail(__FILE__, __LINE__, std::string(::zctest::CurrentTest()) + ": " #cond); \
  } while (0)

#define ZC_CHECK_NEAR(actual, expected, tol)                                                       \
  do {                                                                                             \
    const double a_ = (actual), e_ = (expected), t_ = (tol);                                       \
    if (!(std::fabs(a_ - e_) <= t_)) {                                                             \
      char buf_[256];                                                                              \
      std::snprintf(buf_, sizeof(buf_), "%s: %s = %.6f, expected %.6f +/- %.6f",                  \
                    ::zctest::CurrentTest(), #actual, a_, e_, t_);                                 \
      edge_test::fail(__FILE__, __LINE__, buf_);                                                   \
    }                                                                                              \
  } while (0)

#define ZC_TEST(name) ::zctest::CurrentTest() = name;

void TestEnvelope();
void TestLimiter();
void TestFrameAccumulator();
void TestSampleRing();
void TestFrameRing();
void TestRoomPlan();
void TestReach();
void TestTalkbackEligibility();
void TestDuckPlan();
void TestTalkbackChannels();
void TestSignalGate();
