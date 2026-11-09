#include "TestHarness.h"

#include "modules/ZoomEngineClient.h"

using namespace corevideo::modules;

TEST(EngineProtocol, RosterEventParses) {
  const auto e = parseZoomEngineEvent(
      R"({"cmd":"participants","meeting_generation":1,"roster_revision":4,"active_speaker_id":102,"participants":[)"
      R"({"id":101,"name":"Alice","has_video":true,"is_talking":false,"is_muted":true,"is_host":true,"is_me":false},)"
      R"({"id":200,"name":"CoreVideo Edge","has_video":false,"is_talking":false,"is_muted":true,"is_me":true}]})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->kind, ZoomEngineEventKind::Participants);
  ASSERT_TRUE(e->participants.size() == 2);
  EXPECT_EQ(e->participants[0].displayName, std::string("Alice"));
  EXPECT_TRUE(e->participants[0].hasVideo);
  EXPECT_TRUE(e->participants[1].isMe);
}

TEST(EngineProtocol, FrameBeaconCarriesReceivedSize) {
  const auto e = parseZoomEngineEvent(R"({"cmd":"frame","source_uuid":"edge-s1","participant_id":101,"w":1920,"h":1080})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->kind, ZoomEngineEventKind::Frame);
  EXPECT_EQ(e->sourceUuid, std::string("edge-s1"));
  EXPECT_EQ(e->width, 1920u);
  EXPECT_EQ(e->height, 1080u);
}

TEST(EngineProtocol, JoinFailureReasonIsKept) {
  const auto e = parseZoomEngineEvent(R"({"cmd":"error","stage":"join","msg":"join_failed","reason":"account-busy-elsewhere"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->message, std::string("join_failed: account-busy-elsewhere"));
}

TEST(EngineProtocol, SubscribeAsksFor1080pAndIsolatedAudio) {
  ZoomEngineSubscribeCommand c;
  c.sourceUuid = "edge-s2";
  c.participantId = 101;
  c.resolution = 2;
  const auto video = buildZoomEngineSubscribeCommand(c);
  EXPECT_TRUE(video.find(R"("resolution":2)") != std::string::npos);
  c.isolateAudio = true;
  const auto audio = buildZoomEngineSubscribeAudioCommand(c);
  EXPECT_TRUE(audio.find(R"("cmd":"subscribe_audio")") != std::string::npos);
  EXPECT_TRUE(audio.find(R"("isolate_audio":true)") != std::string::npos);
}
