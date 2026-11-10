#include "edge/EdgeController.h"

#include "core/BoundedAsyncLog.h"
#include "edge/MeetingInput.h"
#include "edge/SteadyClock.h"
#include "edge/JoinFailureText.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace corevideo::edge {
namespace {

using rpc::Json;
using Clock = std::chrono::steady_clock;

constexpr auto kAuthTimeout = std::chrono::seconds(60);
constexpr auto kJoinTimeout = std::chrono::seconds(90);
constexpr auto kEngineSilence = std::chrono::seconds(10);
constexpr auto kReceivingWindow = std::chrono::seconds(3);
constexpr auto kRoomMoveTimeout = std::chrono::seconds(90);
// Zoom SDK MeetingStatus values (meeting_service_interface.h, SDK 7.0.5).
constexpr int kStatusReconnecting = 5;
constexpr int kStatusJoinBreakout = 14;
constexpr int kStatusLeaveBreakout = 15;

// CoreVideo Pro's embedded Meeting SDK public app key
// (native/src/config/ZoomMeetingSdkConfig.h). A PUBLIC client identifier, not a
// secret. Used only when no SDK JWT is supplied, exactly as Pro does.
constexpr const char* kEmbeddedZoomPublicAppKey = "y6sIWSwiTZe1JygMx4C9EQ";

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string firstNonEmpty(const std::string& a, const std::string& b) { return a.empty() ? b : a; }

std::int64_t msSince(Clock::time_point then) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - then).count();
}

}  // namespace

const char* EdgeController::phaseName(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Starting: return "starting-engine";
    case Phase::Authenticating: return "authenticating";
    case Phase::Joining: return "joining";
    case Phase::InMeeting: return "in-meeting";
    case Phase::Leaving: return "leaving";
    case Phase::Failed: return "failed";
  }
  return "unknown";
}

EdgeController::EdgeController(EdgeOptions options, Output output)
    : options_(std::move(options)), output_(std::move(output)) {
  book_.setCap(options_.config.maxSubscriptions);
  book_.setConfigNames(options_.config.subscribe);
  recordFolder_ = options_.config.recordFolder;
  filePrefix_ = options_.config.filePrefix.empty() ? "edge" : options_.config.filePrefix;
  displayName_ = options_.config.displayName.empty() ? defaultDisplayName() : options_.config.displayName;
}

EdgeController::~EdgeController() {
  if (session_.active()) session_.stop();
  preview_.stop();
  feeds_.stopAll();
  engine_.stop();
}

void EdgeController::post(Item item) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back(std::move(item));
  }
  queueCv_.notify_one();
}

void EdgeController::postCommand(std::string line) { post(Command{std::move(line)}); }
void EdgeController::postEndOfInput() { post(EndOfInput{}); }

int EdgeController::run() {
  {
    std::string previewError;
    if (!preview_.start(PreviewPublisher::defaultDir(), previewError))
      warnings_.push_back("Monitor previews unavailable: " + previewError);
  }
  publishState(true);
  while (!quit_) {
    std::deque<Item> batch;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return !queue_.empty(); });
      batch.swap(queue_);
    }
    for (auto& item : batch) {
      if (auto* command = std::get_if<Command>(&item)) handleCommand(command->line);
      else if (auto* notice = std::get_if<EngineNotice>(&item)) handleNotice(*notice);
      else quit_ = true;  // end of input: the parent is gone
      if (quit_) break;
    }
    tick();
    publishState();
  }
  if (session_.active()) stopTake("Edge is shutting down.");
  engine_.stop();
  publishState(true);
  return 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void EdgeController::reply(const std::string& id, const std::string& error) {
  Json::Object object{{"type", "result"}, {"ok", error.empty()}};
  if (!id.empty()) object.emplace("id", id);
  if (!error.empty()) object.emplace("error", error);
  output_(Json(std::move(object)).stringify());
}

void EdgeController::handleCommand(const std::string& line) {
  std::string parseError;
  const auto parsed = Json::parse(line, &parseError);
  if (!parsed || !parsed->isObject()) {
    reply({}, "Command is not a JSON object.");
    return;
  }
  const auto& c = *parsed;
  const auto cmd = c.getString("cmd");
  const auto id = c.getString("id");
  // Never log the line itself: join commands carry tokens.
  core::nativeLogf("[control] %s\n", cmd.c_str());
  std::string error;
  if (cmd == "state") {
    publishState(true);
  } else if (cmd == "join") {
    error = cmdJoin(c);
  } else if (cmd == "leave") {
    error = cmdLeave();
  } else if (cmd == "subscribe") {
    error = cmdSubscribe(c);
  } else if (cmd == "set-fps") {
    error = cmdSetFps(c);
  } else if (cmd == "set-resolution") {
    error = cmdSetResolution(c);
  } else if (cmd == "unsubscribe-slot") {
    error = cmdUnsubscribeSlot(c);
  } else if (cmd == "iso") {
    error = cmdIso(c);
  } else if (cmd == "set-record-folder") {
    if (session_.active()) error = "Stop recording before changing the record folder.";
    else recordFolder_ = trimCopy(c.getString("path"));
  } else if (cmd == "set-file-prefix") {
    if (session_.active()) error = "Stop recording before changing the file prefix.";
    else filePrefix_ = trimCopy(c.getString("prefix")).empty() ? "edge" : trimCopy(c.getString("prefix"));
  } else if (cmd == "engine") {
    error = cmdEngine(c);
  } else if (cmd == "set-feeds") {
    error = cmdSetFeeds(c);
  } else if (cmd == "set-record-format") {
    error = cmdRecordFormat(c);
  } else if (cmd == "record-start") {
    error = cmdRecordStart();
  } else if (cmd == "record-stop") {
    error = cmdRecordStop();
  } else if (cmd == "quit") {
    quit_ = true;
  } else {
    error = "Unknown command: " + cmd;
  }
  reply(id, error);
  markDirty();
}

std::string EdgeController::cmdJoin(const Json& c) {
  if (phase_ != Phase::Idle && phase_ != Phase::Failed) return "Already in or joining a meeting. Leave first.";
  const auto input = parseMeetingInput(c.getString("meeting"), c.getString("passcode"));
  if (!input.ok()) return input.error;
  const auto enginePath = options_.enginePath;
  if (!std::filesystem::exists(enginePath)) {
    return "Zoom engine not found at " + enginePath +
           ". Build it with ZOOM_SDK_DIR set, or start Edge with --fake for the synthetic meeting.";
  }

  PendingJoin join;
  join.meetingId = input.meetingId;
  join.passcode = input.passcode;
  join.displayName = firstNonEmpty(trimCopy(c.getString("displayName")), displayName_);
  join.onBehalfToken = firstNonEmpty(c.getString("onBehalfToken"), env("COREVIDEO_ZOOM_ON_BEHALF_TOKEN"));
  join.userZak = firstNonEmpty(c.getString("userZak"), env("COREVIDEO_ZOOM_USER_ZAK"));
  join.appPrivilegeToken = firstNonEmpty(c.getString("appPrivilegeToken"), env("COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN"));
  const auto* end = c.get("endOtherMeeting");
  join.endOtherMeeting = end && end->asBool(false);

  // SDK auth: a broker-issued JWT (from Sign in with Zoom) replaces the public
  // app key, exactly as in Pro's applyJoinCredentialsFromPayload.
  modules::ZoomEngineInitCommand init;
  init.jwt = firstNonEmpty(c.getString("sdkJwt"), env("COREVIDEO_ZOOM_SDK_JWT"));
  if (init.jwt.empty()) init.publicAppKey = firstNonEmpty(env("COREVIDEO_ZOOM_PUBLIC_APP_KEY"), kEmbeddedZoomPublicAppKey);

  lastError_.clear();
  warnings_.clear();
  canEndOtherSession_ = false;
  movingRooms_ = awaitingArrival_ = false;
  roster_.clear();
  rosterKnown_ = false;
  rawMediaActive_.reset();
  rawMediaReason_.clear();
  received_.clear();
  book_.clear();
  zoomGrant_.clear();
  meetingId_ = join.meetingId;

  phase_ = Phase::Starting;
  phaseSince_ = Clock::now();
  publishState(true);
  std::string error;
  if (!engine_.start(enginePath, [this](EngineNotice notice) { post(std::move(notice)); }, error)) {
    fail(error);
    return error;
  }
  lastEngineLine_ = Clock::now();
  pendingJoin_ = std::move(join);
  phase_ = Phase::Authenticating;
  phaseSince_ = Clock::now();
  if (!engine_.send(modules::buildZoomEngineInitCommand(init))) {
    fail("Could not send the SDK init command to the Zoom engine.");
    return lastError_;
  }
  return {};
}

std::string EdgeController::cmdLeave() {
  movingRooms_ = awaitingArrival_ = false;
  if (phase_ == Phase::Idle) return {};
  if (session_.active()) stopTake("Stopped because the node left the meeting.");
  pendingJoin_.reset();
  if (phase_ == Phase::InMeeting || phase_ == Phase::Joining) {
    phase_ = Phase::Leaving;
    phaseSince_ = Clock::now();
    (void)engine_.send(modules::buildZoomEngineLeaveCommand());
    return {};
  }
  stopEngine();
  phase_ = Phase::Idle;
  return {};
}

std::vector<RosterEntry> EdgeController::rosterEntries() const {
  std::vector<RosterEntry> entries;
  for (const auto& p : roster_) entries.push_back({p.id, p.displayName, p.isMe});
  return entries;
}

bool EdgeController::slotIsRecording(int slot) const {
  if (!session_.active()) return false;
  for (const auto& h : session_.health()) if (h.slot == slot) return true;
  return false;
}

std::string EdgeController::cmdSubscribe(const Json& c) {
  if (phase_ != Phase::InMeeting) return "Join a meeting first.";
  const auto id = static_cast<std::uint32_t>(c.getNumber("participantId"));
  const auto* onValue = c.get("on");
  const bool on = !onValue || onValue->asBool(true);
  const auto it = std::find_if(roster_.begin(), roster_.end(), [&](const auto& p) { return p.id == id; });
  if (it == roster_.end()) return "That participant is not in the meeting.";
  const int slot = static_cast<int>(c.getNumber("slot", 0));
  const auto* current = book_.findByParticipant(id);
  if (!on) {
    if (current && slotIsRecording(current->slot)) {
      return "Stop recording before unsubscribing " + current->displayName + " (their ISO is being recorded).";
    }
  } else if (slot != 0) {
    // A crosspoint take may move this guest or replace an occupant: never
    // under a running ISO on either slot.
    if (current && current->slot != slot && slotIsRecording(current->slot)) {
      return current->displayName + "'s ISO is recording on S" + std::to_string(current->slot) +
             ". Stop recording to move them.";
    }
    if (const auto* occupant = book_.findBySlot(slot);
        occupant && occupant->participantId != id && slotIsRecording(slot)) {
      return "S" + std::to_string(slot) + " is recording " + occupant->displayName + ". Stop recording to replace them.";
    }
  }
  const int height = static_cast<int>(c.getNumber("resolution", 1080));
  const int fps = static_cast<int>(c.getNumber("fps", 0));
  std::vector<SubscriptionAction> actions;
  const auto refusal = book_.request({it->id, it->displayName, it->isMe}, on, actions, "operator", slot, height, fps);
  applyActions(actions);
  return refusal;
}

std::string EdgeController::cmdSetResolution(const Json& c) {
  const int slot = static_cast<int>(c.getNumber("slot"));
  // A re-subscribe drops the slot's frames for a moment and re-creates its
  // engine regions: never under a running ISO (same rule as the ISO selection).
  if (slotIsRecording(slot)) return "The feed resolution is fixed while this slot's ISO is recording. Stop to change it.";
  std::vector<SubscriptionAction> actions;
  const auto refusal = book_.setHeight(slot, static_cast<int>(c.getNumber("resolution", 1080)), actions);
  zoomGrant_.erase(slotSourceUuid(slot));
  applyActions(actions);
  return refusal;
}

std::string EdgeController::cmdSetFps(const Json& c) {
  const int slot = static_cast<int>(c.getNumber("slot"));
  if (slotIsRecording(slot)) return "The frame rate is fixed while this slot's ISO is recording. Stop to change it.";
  const auto refusal = book_.setFps(slot, static_cast<int>(c.getNumber("fps", 0)));
  // tick() hands the routing to the feed sender, which restarts an SRT feed at the new rate.
  return refusal;
}

std::string EdgeController::cmdUnsubscribeSlot(const Json& c) {
  const int slot = static_cast<int>(c.getNumber("slot"));
  if (slotIsRecording(slot)) return "Stop recording before removing a source that is being recorded.";
  std::vector<SubscriptionAction> actions;
  const auto refusal = book_.unsubscribeSlot(slot, actions);
  applyActions(actions);
  return refusal;
}

std::string EdgeController::cmdIso(const Json& c) {
  if (session_.active()) return "The ISO selection is fixed while recording. Stop to change it.";
  const auto* onValue = c.get("on");
  return book_.setIso(static_cast<int>(c.getNumber("slot")), !onValue || onValue->asBool(true));
}

std::string EdgeController::cmdRecordStart() {
  if (session_.active()) return "Already recording.";
  if (phase_ != Phase::InMeeting) return "Join a meeting before recording.";
  IsoSessionRequest request;
  request.folder = recordFolder_;
  request.prefix = filePrefix_;
  request.meetingId = meetingId_;
  request.nodeName = displayName_;
  request.shmPrefix = engine_.shmPrefix();
  request.isoRingEpoch = ++isoRingEpoch_;
  request.writer.videoBitrateMbps = options_.config.videoBitrateMbps;
  request.writer.hardwareEncode = options_.config.hardwareEncode;
  request.writer.height = recordHeight_;
  request.writer.width = recordHeight_ * 16 / 9;
  auto selections = book_.selections();
  std::sort(selections.begin(), selections.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
  for (const auto& s : selections) {
    if (!s.iso) continue;
    if (!s.present) return s.displayName + " has left the meeting. Turn their ISO off or remove them before recording.";
    request.sources.push_back({s.slot, slotSourceUuid(s.slot), s.participantId, s.displayName, s.fps});
  }
  const auto refusal = session_.start(request);
  if (!refusal.empty()) return refusal;
  // Every file is open and every recorder is polling for its ring: arm the
  // engine's per-source ISO arrival rings now. They fill from the next frame,
  // and each recorder maps its ring within one 5 ms poll of it appearing, well
  // inside the ring's 4-frame depth.
  for (const auto& source : request.sources) sendIsoDemand(source.slot, request.isoRingEpoch);
  recordingWarning_.clear();
  lastTakeDir_ = session_.sessionDir().string();
  lastTakeHealth_.clear();
  diskWarning_ = session_.diskWarning();
  lastDiskCheck_ = Clock::now();
  return {};
}

std::string EdgeController::cmdRecordStop() {
  if (!session_.active()) return "Not recording.";
  stopTake({});
  return {};
}

void EdgeController::stopTake(const std::string& reason) {
  if (!session_.active()) return;
  if (!reason.empty()) recordingWarning_ = reason;
  publishState(true);  // operators see "stopping" before the bounded finalize
  const auto slots = session_.health();
  session_.stop();
  for (const auto& h : slots) sendIsoDemand(h.slot, 0);
  lastTakeHealth_ = session_.health();
  diskWarning_ = session_.diskWarning();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void EdgeController::fail(const std::string& message) {
  movingRooms_ = awaitingArrival_ = false;
  core::nativeLogf("[edge] failed: %s\n", message.c_str());
  lastError_ = message;
  if (session_.active()) stopTake("Stopped: " + message);
  stopEngine();
  pendingJoin_.reset();
  phase_ = Phase::Failed;
  phaseSince_ = Clock::now();
  roster_.clear();
  rosterKnown_ = false;
  book_.clear();
  zoomGrant_.clear();
  markDirty();
}

void EdgeController::stopEngine() {
  engine_.stop();
}

void EdgeController::sendSubscribe(int slot, std::uint32_t participantId) {
  modules::ZoomEngineSubscribeCommand command;
  command.sourceUuid = slotSourceUuid(slot);
  command.participantId = participantId;
  // 1080p unless the operator explicitly chose 720p/360p for this slot.
  const auto* selection = book_.findBySlot(slot);
  command.resolution = static_cast<std::uint32_t>(engineResolutionForHeight(selection ? selection->height : 1080));
  (void)engine_.send(modules::buildZoomEngineSubscribeCommand(command));
  command.isolateAudio = true;  // this participant's own audio, before any mix
  (void)engine_.send(modules::buildZoomEngineSubscribeAudioCommand(command));
}

void EdgeController::sendUnsubscribe(int slot) {
  sendIsoDemand(slot, 0);
  (void)engine_.send(modules::buildZoomEngineUnsubscribeCommand(slotSourceUuid(slot)));
  received_.erase(slotSourceUuid(slot));
  zoomGrant_.erase(slotSourceUuid(slot));
}

void EdgeController::sendIsoDemand(int slot, std::uint32_t epoch) {
  Json::Object object{{"cmd", "iso_video"}, {"source_uuid", slotSourceUuid(slot)}, {"epoch", static_cast<double>(epoch)}};
  (void)engine_.send(Json(std::move(object)).stringify());
}

void EdgeController::applyActions(const std::vector<SubscriptionAction>& actions) {
  for (const auto& action : actions) {
    core::nativeLogf("[subscriptions] %s slot %d participant %u (%s)\n",
                     action.kind == SubscriptionAction::Kind::Subscribe ? "subscribe" : "unsubscribe", action.slot,
                     action.participantId, action.reason.c_str());
    if (action.kind == SubscriptionAction::Kind::Subscribe) {
      sendSubscribe(action.slot, action.participantId);
      if (slotIsRecording(action.slot)) {
        // A recorded guest rejoined under a new Zoom id: same slot, same file.
        session_.rebind(action.slot, action.participantId);
        sendIsoDemand(action.slot, isoRingEpoch_);
      }
    } else {
      sendUnsubscribe(action.slot);
    }
  }
  if (!actions.empty()) markDirty();
}

void EdgeController::resubscribeAll() {
  for (const auto& s : book_.selections()) {
    if (!s.present) continue;
    sendSubscribe(s.slot, s.participantId);
    if (slotIsRecording(s.slot)) sendIsoDemand(s.slot, isoRingEpoch_);
  }
}

std::string EdgeController::cmdEngine(const Json& c) {
  const auto* onValue = c.get("on");
  const bool on = !onValue || onValue->asBool(true);
  if (!on && session_.active()) return "Stop recording before turning the engine off (the ISOs would go black).";
  engineOn_ = on;
  if (phase_ != Phase::InMeeting) return {};  // applies when the node is next in a meeting
  // On: (re)request raw data; asks the host for record permission when this
  // account lacks it. Toggling Off then On is the operator's re-request after a
  // breakout move or a denied grant.
  (void)engine_.send(on ? modules::buildZoomEngineStartMediaCommand() : std::string(R"({"cmd":"stop_media"})"));
  return {};
}

std::string EdgeController::cmdSetFeeds(const Json& c) {
  FeedMode mode = feeds_.mode();
  if (c.get("mode") && !parseFeedMode(c.getString("mode"), mode)) return "Feed mode must be off, ndi or srt.";
  SrtSettings srt = feeds_.srt();
  if (c.get("srtHost")) srt.host = trimCopy(c.getString("srtHost"));
  if (c.get("srtBasePort")) srt.basePort = static_cast<int>(c.getNumber("srtBasePort"));
  if (c.get("srtLatencyMs")) srt.latencyMs = static_cast<int>(c.getNumber("srtLatencyMs"));
  if (c.get("srtPassphrase")) srt.passphrase = c.getString("srtPassphrase");  // never logged, never in state
  return feeds_.configure(mode, srt, displayName_);
}

std::string EdgeController::cmdRecordFormat(const Json& c) {
  if (session_.active()) return "Stop recording before changing the record format.";
  const int height = static_cast<int>(c.getNumber("height", recordHeight_));
  if (height != 1080 && height != 720) return "Record resolution must be 1080 or 720.";
  // The frame rate moved to each routed slot (owner 2026-11-09): set-fps.
  if (c.getNumber("fps", 0) != 0) return "The frame rate is set per slot now (set-fps), not on the record format.";
  recordHeight_ = height;
  return {};
}

void EdgeController::handleNotice(const EngineNotice& notice) {
  if (notice.generation != engine_.generation()) return;  // a retired engine
  if (notice.kind == EngineNotice::Kind::Exited) {
    movingRooms_ = awaitingArrival_ = false;
    if (phase_ == Phase::Leaving) {
      phase_ = Phase::Idle;
      stopEngine();
    } else if (phase_ != Phase::Idle && phase_ != Phase::Failed) {
      fail("The Zoom engine stopped unexpectedly (" + notice.reason + ").");
    }
    markDirty();
    return;
  }
  lastEngineLine_ = Clock::now();
  const auto& e = notice.event;
  using K = modules::ZoomEngineEventKind;
  switch (e.kind) {
    case K::AuthOk:
      if (phase_ == Phase::Authenticating && pendingJoin_) {
        modules::ZoomEngineJoinCommand join;
        join.meetingId = pendingJoin_->meetingId;
        join.passcode = pendingJoin_->passcode;
        join.displayName = pendingJoin_->displayName;
        join.onBehalfToken = pendingJoin_->onBehalfToken;
        join.userZak = pendingJoin_->userZak;
        join.appPrivilegeToken = pendingJoin_->appPrivilegeToken;
        join.endOtherMeeting = pendingJoin_->endOtherMeeting;
        auto line = modules::buildZoomEngineJoinCommand(join);
        // Edge delta (zoom-engine README): join camera-off with our mic muted.
        line.insert(line.size() - 1, ",\"capture_only\":true");
        pendingJoin_.reset();  // tokens leave this process's memory as soon as they are sent
        phase_ = Phase::Joining;
        phaseSince_ = Clock::now();
        if (!engine_.send(line)) fail("Could not send the join command to the Zoom engine.");
      }
      break;
    case K::AuthFail:
      fail("Zoom Meeting SDK authentication failed" +
           (e.sdkCode ? " (code " + std::to_string(*e.sdkCode) + ")" : std::string()) +
           ". Sign in with Zoom again, or check COREVIDEO_ZOOM_SDK_JWT.");
      break;
    case K::Joined:
      if (movingRooms_) core::nativeLogf("[meeting] arrived after a room move; re-arming\n");
      movingRooms_ = awaitingArrival_ = false;
      phase_ = Phase::InMeeting;
      phaseSince_ = Clock::now();
      // Engine On: ask for raw data now. If this account may not record, the
      // engine asks the host and re-arms by itself when the host allows it.
      if (engineOn_) (void)engine_.send(modules::buildZoomEngineStartMediaCommand());
      break;
    case K::Left:
      if (movingRooms_ && phase_ == Phase::InMeeting) {
        // A breakout-room move (or a reconnect) is reported as left + joined.
        // Keep the engine, the routing slots and the take: guests re-attach by
        // name when the new room's roster arrives, and ISOs hold their last
        // frame and write silence through the gap (counted, not hidden).
        core::nativeLogf("[meeting] moving rooms: keeping engine, slots and take\n");
        // The arrival timeout runs from here, keyed on this flag (not on the
        // rawMedia reason string, which later engine events overwrite).
        awaitingArrival_ = true;
        movingSince_ = Clock::now();
        roster_.clear();
        rosterKnown_ = false;
        received_.clear();
        rawMediaActive_ = false;
        rawMediaReason_ = "moving_rooms";
        std::vector<SubscriptionAction> none;
        book_.applyRoster({}, none);  // every slot not present until the new room lists them
        break;
      }
      movingRooms_ = awaitingArrival_ = false;
      if (session_.active()) stopTake("Stopped because the meeting ended or this node was removed.");
      // A failed join is followed by `left`; keep the engine's specific reason
      // (live 2026-11-09: "does not admit anonymous apps" was overwritten).
      if (phase_ == Phase::InMeeting) lastError_ = "The meeting ended or this node was removed from it.";
      if (phase_ != Phase::Failed) phase_ = Phase::Idle;
      roster_.clear();
      rosterKnown_ = false;
      book_.clear();
      zoomGrant_.clear();
      received_.clear();
      stopEngine();
      break;
    case K::Participants: {
      roster_ = e.participants;
      rosterKnown_ = true;
      std::vector<SubscriptionAction> actions;
      book_.applyRoster(rosterEntries(), actions);
      applyActions(actions);
      break;
    }
    case K::ActiveSpeaker:
      break;  // talking flags arrive with the roster
    case K::Frame:
      if (!e.sourceUuid.empty() && e.width && e.height) received_[e.sourceUuid] = {e.width, e.height, Clock::now()};
      break;
    case K::RawMediaStatus: {
      const bool wasActive = rawMediaActive_.value_or(false);
      rawMediaActive_ = e.rawMediaActive;
      rawMediaReason_ = e.message;
      // The engine forgets every subscription when raw media stops (Engine
      // Off, a room move, a lost grant). Re-send the routing when it returns.
      if (e.rawMediaActive && !wasActive) resubscribeAll();
      break;
    }
    case K::Error: {
      const bool joinFailure = e.message.rfind("join_failed", 0) == 0 || e.message.rfind("meeting_failed", 0) == 0 ||
                               e.message == "invalid_meeting_id" || e.stage == "join";
      if (joinFailure && (phase_ == Phase::Joining || phase_ == Phase::Authenticating)) {
        canEndOtherSession_ = e.message == "join_failed: account-busy-elsewhere";
        fail(joinFailureText(e.message));
      } else if (e.message.rfind("meeting_failed", 0) == 0 && phase_ == Phase::InMeeting) {
        fail("Zoom ended the meeting connection: " + joinFailureText(e.message));
      } else {
        std::string text = e.stage.empty() ? e.message : e.stage + ": " + (e.message.empty() ? "error" : e.message);
        if (warnings_.size() < 20 && std::find(warnings_.begin(), warnings_.end(), text) == warnings_.end())
          warnings_.push_back(text);
      }
      break;
    }
    case K::Debug:
      if (notice.meetingStatus && (*notice.meetingStatus == kStatusJoinBreakout ||
                                   *notice.meetingStatus == kStatusLeaveBreakout ||
                                   *notice.meetingStatus == kStatusReconnecting)) {
        if (!movingRooms_) core::nativeLogf("[meeting] room move / reconnect (status %d)\n", *notice.meetingStatus);
        movingRooms_ = true;
        movingSince_ = Clock::now();
        break;
      }
      // What the SDK agreed to for a slot's subscription. The engine falls
      // back 1080p -> 720p -> 360p when subscribe() refuses a tier; without
      // this the operator could not tell an SDK refusal from Zoom choosing to
      // send 720p on a granted 1080p subscription.
      if (e.sourceUuid.rfind("edge-s", 0) == 0 &&
          (e.stage == "video_subscribe" || e.stage == "video_resolution_downgraded" ||
           e.stage == "video_subscribe_failed_all")) {
        core::nativeLogf("[zoom-subscribe] %s %s code=%d resolution=%d requested=%d actual=%d\n", e.sourceUuid.c_str(),
                         e.stage.c_str(), e.sdkCode.value_or(-1), e.resolution.value_or(-1),
                         e.requestedResolution.value_or(-1), e.actualResolution.value_or(-1));
        auto& grant = zoomGrant_[e.sourceUuid];
        if (e.stage == "video_subscribe") {
          if (e.sdkCode.value_or(-1) == 0 && e.resolution) {
            grant.grantedHeight = heightForEngineResolution(*e.resolution);
            grant.failed = false;
          } else if (e.sdkCode && e.resolution) {
            grant.lastRefusedHeight = heightForEngineResolution(*e.resolution);
            grant.lastRefusedCode = *e.sdkCode;
          }
        } else if (e.stage == "video_subscribe_failed_all") {
          grant.failed = true;
          grant.grantedHeight = 0;
        }
        break;
      }
      return;
    default:
      return;  // ping: liveness only, no state change
  }
  markDirty();
}

void EdgeController::tick() {
  const auto now = Clock::now();
  if (phase_ == Phase::Authenticating && now - phaseSince_ > kAuthTimeout) {
    fail("Timed out waiting for Zoom Meeting SDK authentication.");
  } else if (phase_ == Phase::Joining && now - phaseSince_ > kJoinTimeout) {
    fail("Timed out waiting for the Zoom meeting join result.");
  } else if (movingRooms_ && !awaitingArrival_ && now - movingSince_ > std::chrono::seconds(30)) {
    // The SDK announced a move / reconnect that never disconnected us: done.
    movingRooms_ = false;
    markDirty();
  } else if (awaitingArrival_ && now - movingSince_ > kRoomMoveTimeout) {
    // Left for a room move but never arrived anywhere: it was a real leave.
    movingRooms_ = awaitingArrival_ = false;
    if (session_.active()) stopTake("Stopped because the node never arrived in the new room.");
    lastError_ = "Zoom moved this node but it never arrived in a room.";
    phase_ = Phase::Idle;
    roster_.clear();
    book_.clear();
    zoomGrant_.clear();
    stopEngine();
    markDirty();
  } else if (phase_ == Phase::Leaving && now - phaseSince_ > std::chrono::seconds(10)) {
    stopEngine();
    phase_ = Phase::Idle;
    markDirty();
  }
  if (session_.active()) {
    if (now - lastDiskCheck_ > std::chrono::seconds(5)) {
      lastDiskCheck_ = now;
      diskWarning_ = session_.diskWarning();
    }
    markDirty();  // health counters move continuously while recording
  }
  if (phase_ == Phase::InMeeting) markDirty();  // "receiving" ages
  // Monitor previews follow the routing.
  std::vector<PreviewSlot> previewSlots;
  if (phase_ == Phase::InMeeting && engine_.running()) {
    for (const auto& s : book_.selections()) previewSlots.push_back({s.slot, slotSourceUuid(s.slot), s.participantId});
  }
  preview_.setSlots(engine_.shmPrefix(), std::move(previewSlots));
  // Live feeds to Pro follow the routing too (only while a mode is on).
  std::vector<FeedSlotSpec> feedSlots;
  if (phase_ == Phase::InMeeting && engine_.running()) {
    for (const auto& s : book_.selections())
      feedSlots.push_back({s.slot, slotSourceUuid(s.slot), s.participantId, s.displayName, s.fps});
  }
  feeds_.setSlots(engine_.shmPrefix(), std::move(feedSlots));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void EdgeController::publishState(bool force) {
  const auto now = Clock::now();
  const auto interval = session_.active() ? std::chrono::milliseconds(500) : std::chrono::milliseconds(250);
  if (!force && (!dirty_ || now - lastPublish_ < interval)) return;
  dirty_ = false;
  lastPublish_ = now;
  output_(buildState().stringify());
}

Json EdgeController::buildState() const {
  Json::Object state{
      {"type", "state"},
      {"seq", static_cast<double>(++stateSeq_)},
      {"version", EDGE_VERSION},
      {"engineKind", options_.fakeEngine ? "fake" : "zoom"},
      {"phase", phaseName(phase_)},
      {"displayName", displayName_},
      {"maxSubscriptions", book_.cap()},
      {"recordFolder", recordFolder_},
      {"filePrefix", filePrefix_},
  };
  state.emplace("engineOn", engineOn_);
  {
    const auto srt = feeds_.srt();
    Json::Object feeds{{"mode", feedModeName(feeds_.mode())}};
    if (!srt.host.empty()) {
      feeds.emplace("srtHost", srt.host);
      feeds.emplace("srtBasePort", static_cast<double>(srt.basePort));
      feeds.emplace("srtLatencyMs", static_cast<double>(srt.latencyMs));
      feeds.emplace("srtEncrypted", !srt.passphrase.empty());
    }
    Json::Array slots;
    for (const auto& h : feeds_.health()) {
      Json::Object row{{"slot", h.slot}, {"target", h.target}, {"state", h.state},
                       {"framesSent", static_cast<double>(h.framesSent)},
                       {"framesSkipped", static_cast<double>(h.framesSkipped)},
                       {"audioSeconds", static_cast<double>(h.audioSeconds)}};
      if (h.receivers >= 0) row.emplace("receivers", h.receivers);
      if (feeds_.mode() == FeedMode::Srt) row.emplace("connected", h.connected);
      if (!h.error.empty()) row.emplace("error", h.error);
      slots.emplace_back(std::move(row));
    }
    feeds.emplace("slots", Json(std::move(slots)));
    state.emplace("feeds", Json(std::move(feeds)));
  }
  if (movingRooms_) state.emplace("movingRooms", true);
  state.emplace("recordFormat", Json(Json::Object{{"width", static_cast<double>(recordHeight_ * 16 / 9)},
                                                  {"height", static_cast<double>(recordHeight_)}}));
  if (!lastError_.empty()) state.emplace("error", lastError_);
  if (canEndOtherSession_) state.emplace("canEndOtherSession", true);
  if (!meetingId_.empty() && phase_ != Phase::Idle) state.emplace("meetingId", meetingId_);
  if (!options_.configPath.empty()) state.emplace("configPath", options_.configPath);
  if (!options_.configErrors.empty()) {
    Json::Array errors;
    for (const auto& e : options_.configErrors) errors.emplace_back(e);
    state.emplace("configErrors", Json(std::move(errors)));
  }
  if (!book_.configWarning().empty()) state.emplace("configWarning", book_.configWarning());
  if (!warnings_.empty()) {
    Json::Array list;
    for (const auto& w : warnings_) list.emplace_back(w);
    state.emplace("engineWarnings", Json(std::move(list)));
  }
  if (phase_ == Phase::InMeeting && lastEngineLine_ != Clock::time_point{} &&
      Clock::now() - lastEngineLine_ > kEngineSilence) {
    state.emplace("engineSilentMs", static_cast<double>(msSince(lastEngineLine_)));
  }
  if (rawMediaActive_) {
    Json::Object raw{{"active", *rawMediaActive_}};
    if (!rawMediaReason_.empty()) raw.emplace("reason", rawMediaReason_);
    state.emplace("rawMedia", Json(std::move(raw)));
  }

  // Participants: the roster as the SDK reports it, joined with this node's
  // subscriptions and the resolution actually being received.
  const auto previewMeasures = preview_.measures();
  if (!preview_.dir().empty()) state.emplace("previewDir", preview_.dir().string());
  Json::Array participants;
  for (const auto& p : roster_) {
    Json::Object row{
        {"id", static_cast<double>(p.id)},
        {"name", p.displayName},
        {"videoOn", p.hasVideo},
        {"talking", p.isTalking},
        {"muted", p.isMuted},
        {"host", p.isHost},
    };
    if (p.isMe) row.emplace("self", true);
    if (const auto* s = book_.findByParticipant(p.id)) {
      row.emplace("slot", s->slot);
      row.emplace("iso", s->iso);
      const auto r = received_.find(slotSourceUuid(s->slot));
      if (r != received_.end()) {
        Json::Object rec{{"width", static_cast<double>(r->second.width)},
                         {"height", static_cast<double>(r->second.height)},
                         {"msSinceFrame", static_cast<double>(msSince(r->second.at))},
                         {"receiving", Clock::now() - r->second.at < kReceivingWindow}};
        if (const auto m = previewMeasures.find(s->slot); m != previewMeasures.end()) {
          if (m->second.fpsX100 >= 0) rec.emplace("fps", m->second.fpsX100 / 100.0);
          if (m->second.hasAudio) rec.emplace("audioDb", m->second.audioPeakDbX100 / 100.0);
        }
        row.emplace("received", Json(std::move(rec)));
      }
    }
    participants.emplace_back(std::move(row));
  }
  if (rosterKnown_) state.emplace("participants", Json(std::move(participants)));

  Json::Array selections;
  for (const auto& s : book_.selections()) {
    Json::Object row{{"slot", s.slot}, {"participantId", static_cast<double>(s.participantId)},
                     {"name", s.displayName}, {"present", s.present}, {"iso", s.iso},
                     {"fromConfig", s.fromConfig}, {"resolution", s.height}, {"fps", s.fps}};
    if (const auto g = zoomGrant_.find(slotSourceUuid(s.slot)); g != zoomGrant_.end()) {
      Json::Object zoom;
      if (g->second.grantedHeight) zoom.emplace("granted", g->second.grantedHeight);
      if (g->second.lastRefusedHeight) {
        zoom.emplace("refused", g->second.lastRefusedHeight);
        zoom.emplace("refusedCode", g->second.lastRefusedCode);
      }
      if (g->second.failed) zoom.emplace("failed", true);
      row.emplace("zoom", Json(std::move(zoom)));
    }
    selections.emplace_back(std::move(row));
  }
  state.emplace("subscriptions", Json(std::move(selections)));

  // Recording: live health while a take runs; the last take's final health after.
  const bool active = session_.active();
  const auto healths = active ? session_.health() : lastTakeHealth_;
  Json::Object recording{{"active", active}};
  if (active) {
    recording.emplace("startedUtc", session_.startedUtc());
    recording.emplace("elapsedMs", static_cast<double>((steadyNow100ns() - session_.epoch100ns()) / 10'000));
  }
  if (!lastTakeDir_.empty()) recording.emplace("sessionDir", lastTakeDir_);
  std::string warning = recordingWarning_;
  if (warning.empty()) warning = diskWarning_;
  if (active && rawMediaActive_ && !*rawMediaActive_ && warning.empty()) {
    warning = "Zoom raw data is not active (the host has not allowed this node to record). ISOs hold black and silence until it is.";
  }
  if (!warning.empty()) recording.emplace("warning", warning);
  Json::Array streams;
  for (const auto& h : healths) {
    Json::Object row{
        {"index", h.index},
        {"slot", h.slot},
        {"name", h.displayName},
        {"participantId", static_cast<double>(h.participantId)},
        {"path", h.path},
        {"state", h.state},
        {"videoFrames", static_cast<double>(h.videoFrames)},
        {"videoLost", static_cast<double>(h.videoLost)},
        {"videoDropped", static_cast<double>(h.videoDropped)},
        {"heldRepeats", static_cast<double>(h.heldRepeats)},
        {"encoderBacklogPeak", static_cast<double>(h.encoderBacklogPeak)},
        {"cadenceRepeats", static_cast<double>(h.cadenceRepeats)},
        {"cadenceSkipped", static_cast<double>(h.cadenceSkipped)},
        {"audioSamples", static_cast<double>(h.audioSamples)},
        {"silenceSamples", static_cast<double>(h.silenceSamples)},
        {"audioLostPackets", static_cast<double>(h.audioLostPackets)},
    };
    if (!h.encoder.empty()) row.emplace("encoder", h.encoder);
    if (h.lastWidth) {
      row.emplace("width", static_cast<double>(h.lastWidth));
      row.emplace("height", static_cast<double>(h.lastHeight));
    }
    if (h.msSinceVideo >= 0) row.emplace("msSinceVideo", static_cast<double>(h.msSinceVideo));
    if (h.msSinceAudio >= 0) row.emplace("msSinceAudio", static_cast<double>(h.msSinceAudio));
    if (h.bytes > 0) row.emplace("bytes", static_cast<double>(h.bytes));
    std::string streamWarning = h.warning;
    if (streamWarning.empty() && h.videoDropped > 0)
      streamWarning = "Encoder fell behind: " + std::to_string(h.videoDropped) + " frame(s) dropped.";
    if (streamWarning.empty() && active && h.state == "recording") {
      if (h.msSinceVideo < 0 && (steadyNow100ns() - session_.epoch100ns()) > 50'000'000)
        streamWarning = "No video received yet.";
      else if (h.msSinceVideo > 3000)
        streamWarning = "No video for " + std::to_string(h.msSinceVideo / 1000) + " s.";
    }
    if (!streamWarning.empty()) row.emplace("warning", streamWarning);
    streams.emplace_back(std::move(row));
  }
  recording.emplace("streams", Json(std::move(streams)));
  state.emplace("recording", Json(std::move(recording)));
  return Json(std::move(state));
}

}  // namespace corevideo::edge
