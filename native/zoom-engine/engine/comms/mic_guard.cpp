#include "mic_guard.h"

namespace edgecomms {

void MicGuard::Reset() {
  installed_ = owns_ = seen_initialised_ = gave_up_ = false;
  logged_install_failure_ = logged_no_licence_ = logged_operator_muted_ = false;
  installed_at_ms_ = retry_at_ms_ = next_unmute_ms_ = 0;
}

std::vector<std::string> MicGuard::TakeLog() {
  std::vector<std::string> out;
  out.swap(log_);
  return out;
}

void MicGuard::GiveUp(const std::string& why) {
  owns_ = false;
  gave_up_ = true;
  Log(why + " -- meeting mic MUTED so the room never hears this PC; talkback is silent for this meeting");
}

bool MicGuard::Dropped() {
  seen_initialised_ = false;
  if (!owns_) return false;
  GiveUp("silent virtual mic was dropped by Zoom");
  return true;
}

void MicGuard::InstallResult(bool ok, const std::string& detail, int64_t now_ms) {
  if (ok) {
    installed_ = true;
    owns_ = true;  // provisional: the watchdog below decides
    installed_at_ms_ = now_ms;
    next_unmute_ms_ = 0;
    Log("meeting mic auto-suppressed (open but silent to the room)");
    return;
  }
  retry_at_ms_ = now_ms + kRetryMs;
  if (!logged_install_failure_) {
    logged_install_failure_ = true;
    Log("mic auto-suppress unavailable (" + detail +
        ") -- the meeting mic stays closed, talkback is silent; retrying every 5 s");
  }
}

MicGuard::Act MicGuard::Next(const In& in) {
  if (!in.in_meeting) {
    if (installed_ || owns_ || gave_up_) Reset();
    return Act::None;
  }
  if (!in.capture_only) {
    // An operator who speaks in the meeting keeps their own mic. Never
    // installed, never owned; only say when their mute silences talkback.
    if (in.can_talkback && in.self_muted && !logged_operator_muted_) {
      logged_operator_muted_ = true;
      Log("your meeting mic is muted -- talkback is silent until you unmute in Zoom");
    }
    if (!in.self_muted) logged_operator_muted_ = false;
    return Act::None;
  }
  // Watchdog first: it is the only path that closes an exposed mic.
  if (owns_) {
    if (in.initialised) seen_initialised_ = true;
    if (seen_initialised_ && !in.initialised) {
      GiveUp("silent virtual mic stopped");
      return Act::Revoke;
    }
    if (!seen_initialised_ && in.now_ms - installed_at_ms_ > kInitGraceMs) {
      GiveUp("silent virtual mic never initialised (no raw-data entitlement?)");
      return Act::Revoke;
    }
  }
  if (gave_up_ || !in.can_talkback) return Act::None;
  if (!installed_) {
    if (in.now_ms < retry_at_ms_) return Act::None;
    if (!in.licence) {
      retry_at_ms_ = in.now_ms + kRetryMs;
      if (!logged_no_licence_) {
        logged_no_licence_ = true;
        Log("no raw-data licence for this sign-in: the meeting mic stays closed (the room must never hear this PC), "
            "so talkback is silent");
      }
      return Act::None;
    }
    return Act::Install;
  }
  if (owns_ && in.self_muted && in.now_ms >= next_unmute_ms_) {
    next_unmute_ms_ = in.now_ms + kUnmuteEveryMs;
    return Act::Unmute;
  }
  return Act::None;
}

}  // namespace edgecomms
