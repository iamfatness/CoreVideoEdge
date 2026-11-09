#pragma once

// Operator language for a failed join. `join_failed: <reason>` (the SDK's
// join-time prompts) goes through CoreVideo Pro's zoomJoinFailureMessage
// unchanged; this adds the common `meeting_failed: <MEETING_FAIL_*>` codes the
// engine reports (zoom-engine/engine/main.cpp meeting_fail_name). Same rule as
// Pro: anything unrecognised is shown VERBATIM, never turned into a vague
// sentence.

#include "modules/ZoomJoinFailureMessage.h"

#include <string>

namespace corevideo::edge {

inline std::string joinFailureText(const std::string& raw) {
  constexpr const char* kPrefix = "meeting_failed: ";
  const std::string prefix(kPrefix);
  if (raw.compare(0, prefix.size(), prefix) != 0) return modules::zoomJoinFailureMessage(raw);
  const auto code = raw.substr(prefix.size());
  if (code == "MEETING_FAIL_MEETING_NOT_EXIST") return "Zoom says that meeting does not exist. Check the meeting link.";
  if (code == "MEETING_FAIL_PASSWORD_ERR") return "Zoom rejected the meeting passcode. Use the full join link with its pwd=.";
  if (code == "MEETING_FAIL_MEETING_NOT_START") return "The meeting has not started yet.";
  if (code == "MEETING_FAIL_MEETING_OVER") return "The meeting has already ended.";
  if (code == "MEETING_FAIL_MEETING_USER_FULL") return "The meeting is full.";
  if (code == "MEETING_FAIL_CONFLOCKED") return "The host has locked the meeting.";
  if (code == "MEETING_FAIL_ENFORCE_LOGIN" || code == "MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING")
    return "This meeting only admits signed-in Zoom users. Sign in with Zoom on this node and join again.";
  if (code == "MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN" || code == "MEETING_FAIL_FORBID_TO_JOIN_INTERNAL_MEETING")
    return "The host's account does not allow this node's Zoom account to join.";
  if (code == "MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING")
    return "This meeting does not admit anonymous apps. Sign in with Zoom on this node and join again.";
  if (code == "MEETING_FAIL_CONNECTION_ERR" || code == "MEETING_FAIL_RECONNECT_ERR")
    return "Could not connect to Zoom. Check this machine's network.";
  return raw;
}

}  // namespace corevideo::edge
