// The rule that the station's REAL mic can never be open to the room
// (mic_guard.h). The engine performs what the guard decides; these pin the
// decisions.
#include "TestHarness.h"

#include "mic_guard.h"

using namespace edgecomms;
using Act = MicGuard::Act;

namespace {

MicGuard::In ready(int64_t now) {
  MicGuard::In in;
  in.in_meeting = true;
  in.can_talkback = true;
  in.capture_only = true;
  in.licence = true;
  in.initialised = false;
  in.self_muted = true;
  in.now_ms = now;
  return in;
}

}  // namespace

TEST(MicGuard, NoRawDataLicenceMeansNoInstallAndTheMicStaysClosed) {
  // ZComms: setExternalAudioSource can SUCCEED and never take effect without
  // the raw-data entitlement -- then an unmute would broadcast this PC.
  MicGuard g;
  auto in = ready(1000);
  in.licence = false;
  for (int i = 0; i < 100; ++i, in.now_ms += 100) EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  const auto log = g.TakeLog();
  EXPECT_EQ(log.size(), size_t{1});  // said once, not every tick
}

TEST(MicGuard, InstallOwnsProvisionallyAndUnmutesWithoutWaitingForInit) {
  MicGuard g;
  auto in = ready(1000);
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(true, "ok", in.now_ms);
  EXPECT_TRUE(g.owns());  // capture-only stands down...
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::Unmute);  // ...and the unmute is not gated on init
  in.now_ms += 100;
  in.initialised = true;
  EXPECT_TRUE(g.Next(in) == Act::None);  // re-asserted only every 2 s
  in.now_ms += 2100;
  EXPECT_TRUE(g.Next(in) == Act::Unmute);
  in.self_muted = false;
  in.now_ms += 2100;
  EXPECT_TRUE(g.Next(in) == Act::None);  // open: nothing to do
  EXPECT_TRUE(g.owns());
}

TEST(MicGuard, NoInitialiseWithinGraceRevokesAndNeverRetriesThatMeeting) {
  MicGuard g;
  auto in = ready(1000);
  g.Next(in);
  g.InstallResult(true, "ok", in.now_ms);
  in.now_ms += 1400;
  g.Next(in);  // unmute, still inside the grace window
  EXPECT_TRUE(g.owns());
  in.now_ms += 200;  // 1.6 s, never initialised
  EXPECT_TRUE(g.Next(in) == Act::Revoke);
  EXPECT_FALSE(g.owns());
  EXPECT_TRUE(g.gave_up());
  // A retry would re-open the real mic for another grace window: never.
  for (int i = 0; i < 200; ++i) {
    in.now_ms += 100;
    const Act a = g.Next(in);
    EXPECT_TRUE(a != Act::Install && a != Act::Unmute);
  }
  // A new meeting starts clean.
  g.Reset();
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::Install);
}

TEST(MicGuard, ADroppedVirtualMicRevokesImmediately) {
  MicGuard g;
  auto in = ready(1000);
  g.Next(in);
  g.InstallResult(true, "ok", in.now_ms);
  in.initialised = true;
  in.now_ms += 100;
  g.Next(in);
  EXPECT_TRUE(g.owns());
  // The flag going false (polled) ...
  in.initialised = false;
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::Revoke);
  EXPECT_FALSE(g.owns());
  // ... or onMicUninitialized (pushed) on a fresh install.
  MicGuard h;
  auto hi = ready(1000);
  h.Next(hi);
  h.InstallResult(true, "ok", hi.now_ms);
  EXPECT_TRUE(h.Dropped());  // the caller mutes now
  EXPECT_FALSE(h.owns());
  EXPECT_FALSE(h.Dropped());  // nothing owned any more: nothing to mute
}

TEST(MicGuard, ASynchronousInstallRefusalIsRetriedEveryFiveSecondsAndLoggedOnce) {
  MicGuard g;
  auto in = ready(1000);
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(false, "setExternalAudioSource 3", in.now_ms);
  EXPECT_FALSE(g.owns());
  in.now_ms += 4900;
  EXPECT_TRUE(g.Next(in) == Act::None);
  in.now_ms += 200;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(false, "setExternalAudioSource 3", in.now_ms);
  EXPECT_EQ(g.TakeLog().size(), size_t{1});
  in.now_ms += 5100;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(true, "ok", in.now_ms);
  EXPECT_TRUE(g.owns());
}

TEST(MicGuard, AnOperatorWhoSpeaksInTheMeetingIsNeverTouched) {
  // A join that is not capture-only (Pro's operator): no virtual mic, no
  // unmute, ever; only a report when their own mute silences talkback.
  MicGuard g;
  auto in = ready(1000);
  in.capture_only = false;
  for (int i = 0; i < 100; ++i, in.now_ms += 100) EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  EXPECT_EQ(g.TakeLog().size(), size_t{1});
}

TEST(MicGuard, NothingWithoutTalkbackAndLeavingDropsOwnership) {
  MicGuard g;
  auto in = ready(1000);
  in.can_talkback = false;  // not host/co-host, or unsupported
  EXPECT_TRUE(g.Next(in) == Act::None);
  in.can_talkback = true;
  EXPECT_TRUE(g.Next(in) == Act::Install);
  g.InstallResult(true, "ok", in.now_ms);
  in.in_meeting = false;
  in.now_ms += 100;
  EXPECT_TRUE(g.Next(in) == Act::None);
  EXPECT_FALSE(g.owns());
  EXPECT_FALSE(g.installed());
}
