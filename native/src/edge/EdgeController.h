#pragma once

// The Edge node: one Zoom meeting, the participants this node subscribes to,
// and their ISO take. Driven by JSON-line commands; publishes JSON-line state.
// This is Edge's control API - the operator app and the headless validator
// speak the same protocol (docs/PROTOCOL.md).
//
// Everything published is measured or reported by the engine/SDK at the point
// it happens. A value Edge has not observed is ABSENT from the state, never a
// placeholder (CoreVideo Pro docs/reference/honest-reporting.md).

#include "edge/EdgeConfig.h"
#include "edge/EngineSupervisor.h"
#include "edge/IsoSession.h"
#include "edge/FeedSender.h"
#include "edge/PreviewPublisher.h"
#include "edge/SubscriptionPolicy.h"
#include "rpc/Json.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace corevideo::edge {

struct EdgeOptions {
  std::string enginePath;
  bool fakeEngine = false;
  EdgeConfig config;
  std::vector<std::string> configErrors;
  std::string configPath;
};

class EdgeController {
 public:
  using Output = std::function<void(const std::string& line)>;

  EdgeController(EdgeOptions options, Output output);
  ~EdgeController();

  // Thread-safe: queue a command line (from stdin) for the controller thread.
  void postCommand(std::string line);
  // Runs until a quit command or end of input. Returns the process exit code.
  int run();
  void postEndOfInput();

 private:
  struct Command { std::string line; };
  struct EndOfInput {};
  using Item = std::variant<Command, EngineNotice, EndOfInput>;

  enum class Phase { Idle, Starting, Authenticating, Joining, InMeeting, Leaving, Failed };
  static const char* phaseName(Phase phase);

  void post(Item item);
  void handleCommand(const std::string& line);
  void handleNotice(const EngineNotice& notice);
  void tick();

  // commands
  std::string cmdJoin(const rpc::Json& c);
  std::string cmdLeave();
  std::string cmdSubscribe(const rpc::Json& c);
  std::string cmdUnsubscribeSlot(const rpc::Json& c);
  std::string cmdIso(const rpc::Json& c);
  std::string cmdRecordStart();
  std::string cmdRecordStop();
  std::string cmdEngine(const rpc::Json& c);
  std::string cmdRecordFormat(const rpc::Json& c);
  std::string cmdSetFeeds(const rpc::Json& c);
  void resubscribeAll();

  void fail(const std::string& message);
  void stopEngine();
  void applyActions(const std::vector<SubscriptionAction>& actions);
  void sendSubscribe(int slot, std::uint32_t participantId);
  void sendUnsubscribe(int slot);
  void sendIsoDemand(int slot, std::uint32_t epoch);
  void stopTake(const std::string& reason);
  std::vector<RosterEntry> rosterEntries() const;
  bool slotIsRecording(int slot) const;

  void markDirty() { dirty_ = true; }
  void publishState(bool force = false);
  rpc::Json buildState() const;
  void reply(const std::string& id, const std::string& error);

  EdgeOptions options_;
  Output output_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<Item> queue_;
  bool quit_ = false;

  EngineSupervisor engine_;
  Phase phase_ = Phase::Idle;
  std::chrono::steady_clock::time_point phaseSince_{};
  std::chrono::steady_clock::time_point lastEngineLine_{};
  std::string lastError_;
  std::vector<std::string> warnings_;  // engine-reported non-fatal problems this meeting
  bool canEndOtherSession_ = false;

  struct PendingJoin {
    std::string meetingId, passcode, displayName, onBehalfToken, userZak, appPrivilegeToken;
    bool endOtherMeeting = false;
  };
  std::optional<PendingJoin> pendingJoin_;  // dropped once the engine has it
  std::string meetingId_;
  std::string displayName_;

  std::vector<modules::ZoomEngineParticipant> roster_;
  bool rosterKnown_ = false;
  std::optional<bool> rawMediaActive_;
  std::string rawMediaReason_;
  bool engineOn_ = true;          // operator's Engine On: raw media requested while in a meeting
  bool movingRooms_ = false;      // SDK reported a breakout move / reconnect; `left` is not the end
  bool awaitingArrival_ = false;  // left for a move, not yet joined the new room (90 s limit)
  std::chrono::steady_clock::time_point movingSince_{};
  int recordHeight_ = 1080;       // 1080 | 720 (16:9)
  int recordFps_ = 0;             // 0 = as Zoom delivers (VFR), 30 | 60 = constant
  struct Received { std::uint32_t width = 0, height = 0; std::chrono::steady_clock::time_point at; };
  std::map<std::string, Received> received_;  // by slot source_uuid, from engine frame beacons

  SubscriptionBook book_;
  std::string recordFolder_;
  std::string filePrefix_;

  IsoSession session_;
  PreviewPublisher preview_;
  FeedSender feeds_;
  std::uint32_t isoRingEpoch_ = 0;
  std::string recordingWarning_;
  std::string lastTakeDir_;
  std::vector<IsoSourceHealth> lastTakeHealth_;
  std::chrono::steady_clock::time_point lastDiskCheck_{};
  std::string diskWarning_;

  bool dirty_ = true;
  mutable std::uint64_t stateSeq_ = 0;
  std::chrono::steady_clock::time_point lastPublish_{};
};

}  // namespace corevideo::edge
