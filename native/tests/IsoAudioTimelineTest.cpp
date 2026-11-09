#include "TestHarness.h"

#include "edge/IsoAudioTimeline.h"

using corevideo::edge::IsoAudioTimeline;

namespace {
constexpr std::int64_t kMs = 10'000;  // 100 ns ticks per ms
}

TEST(IsoAudioTimeline, LeadingSilenceAlignsTheFirstWordToTheEpoch) {
  IsoAudioTimeline t(48000);
  // First 10 ms packet read 2.010 s after the epoch: it began at 2.000 s.
  const auto p = t.place(2010 * kMs, 480);
  EXPECT_EQ(p.silenceFrames, std::int64_t{96000});
  EXPECT_EQ(p.startFrame, std::int64_t{96000});
}

TEST(IsoAudioTimeline, ContinuousSpeechWithPollJitterDoesNotDrift) {
  IsoAudioTimeline t(48000);
  std::int64_t now = 0;
  std::int64_t silence = 0;
  // 60 s of 10 ms packets, read in irregular batches (5..35 ms late).
  const int jitter[] = {5, 20, 35, 10, 0, 25};
  std::int64_t produced = 0;
  for (int i = 0; produced < 48000 * 60; ++i) {
    const std::int64_t batchMs = 10 + jitter[i % 6];
    now += batchMs * kMs;
    const std::int64_t frames = 48 * batchMs;
    silence += t.place(now, frames).silenceFrames;
    produced += frames;
  }
  EXPECT_EQ(silence, std::int64_t{0});
  EXPECT_EQ(t.writtenFrames(), produced);
}

TEST(IsoAudioTimeline, APauseInSpeechIsFilledWithSilence) {
  IsoAudioTimeline t(48000);
  t.place(1000 * kMs, 48000);  // 0..1 s of speech
  // Next packet read at 3.010 s: 2 s of pause.
  const auto p = t.place(3010 * kMs, 480);
  EXPECT_EQ(p.silenceFrames, std::int64_t{96000});
  EXPECT_EQ(p.startFrame, std::int64_t{144000});
}

TEST(IsoAudioTimeline, SmallGapsInsideWordsAreNotFilled) {
  IsoAudioTimeline t(48000);
  t.place(1000 * kMs, 48000);
  const auto p = t.place(1030 * kMs, 480);  // 20 ms late: delivery jitter, not a pause
  EXPECT_EQ(p.silenceFrames, std::int64_t{0});
}

TEST(IsoAudioTimeline, NeverRewindsAndPadsToTheTakeEnd) {
  IsoAudioTimeline t(48000);
  t.place(100 * kMs, 48000);  // a burst larger than the elapsed time
  EXPECT_EQ(t.writtenFrames(), std::int64_t{48000});
  const auto p = t.place(200 * kMs, 480);
  EXPECT_EQ(p.silenceFrames, std::int64_t{0});
  EXPECT_EQ(p.startFrame, std::int64_t{48000});
  EXPECT_EQ(t.padTo(96000), std::int64_t{96000 - 48480});
  EXPECT_EQ(t.padTo(10), std::int64_t{0});
}

// Live 2026-11-09: a guest who never speaks sends no packets; without catch-up
// the stem never advanced and the MP4 muxer held every video frame (50 GB).
TEST(IsoAudioTimeline, ASilentGuestsStemKeepsUpWithRealTime) {
  IsoAudioTimeline t(48000);
  std::int64_t silence = 0;
  for (std::int64_t ms = 5; ms <= 60'000; ms += 5) silence += t.catchUp(ms * kMs);
  // Within holdback (200 ms) + tolerance (40 ms) of real time, never ahead of it.
  EXPECT_TRUE(silence >= 48 * (60'000 - 240));
  EXPECT_TRUE(silence <= 48 * (60'000 - 200));
  EXPECT_EQ(t.writtenFrames(), silence);
}

TEST(IsoAudioTimeline, CatchUpWritesChunksNotSlivers) {
  IsoAudioTimeline t(48000);
  EXPECT_EQ(t.catchUp(230 * kMs), std::int64_t{0});  // 30 ms past the holdback: under tolerance
  EXPECT_EQ(t.catchUp(260 * kMs), std::int64_t{48 * 60});
}

TEST(IsoAudioTimeline, CatchUpAddsNoSilenceInsideContinuousSpeech) {
  IsoAudioTimeline t(48000);
  std::int64_t silence = 0, produced = 0, now = 0;
  const int jitter[] = {5, 20, 35, 10, 0, 25};
  for (int i = 0; produced < 48000 * 60; ++i) {
    const std::int64_t batchMs = 10 + jitter[i % 6];
    now += batchMs * kMs;
    silence += t.place(now, 48 * batchMs).silenceFrames;
    silence += t.catchUp(now + 3 * kMs);  // the poll after a batch, before the next one
    produced += 48 * batchMs;
  }
  EXPECT_EQ(silence, std::int64_t{0});
}

TEST(IsoAudioTimeline, APacketLateWithinTheHoldbackIsStillPlacedExactly) {
  IsoAudioTimeline t(48000);
  (void)t.catchUp(2000 * kMs);  // silent until 2 s: stem filled to 1.8 s
  // A word that began at 1.85 s, read at 1.86 s + 100 ms delivery delay.
  const auto p = t.place(1960 * kMs, 480 * 1);  // ends at 1.96 s read time
  EXPECT_EQ(p.startFrame, std::int64_t{48 * 1950});
}
