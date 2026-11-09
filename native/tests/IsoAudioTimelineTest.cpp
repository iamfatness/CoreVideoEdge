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
