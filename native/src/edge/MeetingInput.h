#pragma once

// Turns what the operator pasted (a Zoom join link or a bare meeting ID) into
// the meeting number and passcode the engine's `join` takes. The parsing rules
// are CoreVideo Pro's (modules/ZoomMeetingId.h); this only adds trimming and
// the operator-facing refusal text.

#include "modules/ZoomMeetingId.h"

#include <string>

namespace corevideo::edge {

struct MeetingInput {
  std::string meetingId;
  std::string passcode;
  std::string error;  // non-empty: refuse the join, show this to the operator
  [[nodiscard]] bool ok() const { return error.empty(); }
};

inline std::string trimCopy(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// `explicitPasscode` wins over a pwd= in the link (same rule as Pro).
inline MeetingInput parseMeetingInput(const std::string& rawInput, const std::string& explicitPasscode = {}) {
  MeetingInput out;
  const auto input = trimCopy(rawInput);
  if (input.empty()) {
    out.error = "Paste a Zoom meeting link or meeting ID.";
    return out;
  }
  const bool looksLikeUrl = input.find("://") != std::string::npos || input.find("zoom.us") != std::string::npos ||
                            input.find('/') != std::string::npos;
  if (looksLikeUrl) {
    out.meetingId = modules::meetingIdFromJoinInput({}, input);
    out.passcode = modules::passcodeFromJoinUrl(input);
  } else {
    out.meetingId = modules::meetingIdFromJoinInput(input, {});
  }
  if (!explicitPasscode.empty()) out.passcode = trimCopy(explicitPasscode);
  if (out.meetingId.size() < 9 || out.meetingId.size() > 12) {
    out.meetingId.clear();
    out.error = looksLikeUrl
        ? "That link has no Zoom meeting number. Use the full https://...zoom.us/j/<number>?pwd=... link."
        : "A Zoom meeting ID is 9 to 12 digits.";
  }
  return out;
}

}  // namespace corevideo::edge
