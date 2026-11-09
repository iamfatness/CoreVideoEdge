#include "TestHarness.h"

#include "edge/MeetingInput.h"

using corevideo::edge::parseMeetingInput;

TEST(MeetingInput, FullJoinLinkKeepsItsPasscode) {
  const auto m = parseMeetingInput("https://us02web.zoom.us/j/81234567890?pwd=AbCdEf123.1");
  EXPECT_TRUE(m.ok());
  EXPECT_EQ(m.meetingId, std::string("81234567890"));
  EXPECT_EQ(m.passcode, std::string("AbCdEf123.1"));
}

TEST(MeetingInput, HostAndPasscodeDigitsAreNotTheMeetingNumber) {
  const auto m = parseMeetingInput("  https://us06web.zoom.us/j/9876543210?pwd=123456789#success \n");
  EXPECT_EQ(m.meetingId, std::string("9876543210"));
  EXPECT_EQ(m.passcode, std::string("123456789"));
}

TEST(MeetingInput, BareIdWithSpacesOrDashes) {
  EXPECT_EQ(parseMeetingInput("987 654 3210").meetingId, std::string("9876543210"));
  EXPECT_EQ(parseMeetingInput("987-654-3210").meetingId, std::string("9876543210"));
}

TEST(MeetingInput, ExplicitPasscodeWinsOverTheLink) {
  const auto m = parseMeetingInput("https://zoom.us/j/81234567890?pwd=fromlink", "typed");
  EXPECT_EQ(m.passcode, std::string("typed"));
}

TEST(MeetingInput, RefusesWhatIsNotAMeeting) {
  EXPECT_FALSE(parseMeetingInput("").ok());
  EXPECT_FALSE(parseMeetingInput("hello").ok());
  EXPECT_FALSE(parseMeetingInput("12345").ok());
  EXPECT_FALSE(parseMeetingInput("https://zoom.us/meeting/schedule").ok());
}

#include "edge/JoinFailureText.h"

TEST(JoinFailureText, CommonMeetingFailuresReadAsOperatorLanguage) {
  using corevideo::edge::joinFailureText;
  EXPECT_EQ(joinFailureText("meeting_failed: MEETING_FAIL_MEETING_NOT_EXIST"),
            std::string("Zoom says that meeting does not exist. Check the meeting link."));
  EXPECT_TRUE(joinFailureText("join_failed: account-busy-elsewhere").find("already in this meeting") != std::string::npos);
  // Unknown codes pass through verbatim, never a vague sentence.
  EXPECT_EQ(joinFailureText("meeting_failed: MEETING_FAIL_SOMETHING_NEW"), std::string("meeting_failed: MEETING_FAIL_SOMETHING_NEW"));
}
