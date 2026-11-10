#pragma once

// Which meeting participants this Edge node pulls from Zoom, and nothing else.
//
// Product rule (owner, 2026-11-08): an Edge exists to spend its OWN Zoom
// downlink budget on the guests CoreVideo Pro is not pulling. So:
//   - never auto-subscribe the meeting; only explicit operator picks and names
//     listed in the node's config file are subscribed;
//   - a hard cap (default 8 simultaneous subscriptions) is enforced here,
//     not in the UI, so no caller can exceed it;
//   - resolution is never traded for count automatically: every subscription
//     asks for 1080p unless the operator explicitly picks 720p or 360p for
//     that slot (owner, 2026-11-09).
//
// A selection owns a stable SLOT (edge-s1..edge-sN). The slot, not the Zoom
// user id, names the engine's shared-memory regions. When a selected guest
// drops and rejoins (Zoom hands them a NEW user id), the slot is re-bound to the
// new id under the same name, so an ISO being recorded keeps writing to the
// same file instead of losing the guest for the rest of the take.
//
// Pure: no engine, no clock, no I/O. The controller turns Actions into engine
// commands.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::edge {

inline constexpr int kDefaultMaxSubscriptions = 8;
// The engine's ISO arrival-ring demand table holds 8 sources
// (zoom-engine/engine/iso-video-demand.h); a config may not exceed it.
inline constexpr int kHardMaxSubscriptions = 8;

struct RosterEntry {
  std::uint32_t id = 0;
  std::string name;
  bool isMe = false;
};

struct Selection {
  int slot = 0;                   // 1..cap, stable for the life of the selection
  std::uint32_t participantId = 0;
  std::string displayName;        // name at selection; the rebind key
  bool present = true;            // false: the guest left; slot held for a rejoin
  bool iso = true;                // record an ISO for this source
  bool fromConfig = false;
  int height = 1080;              // Zoom feed requested: 1080 | 720 | 360
  int fps = 0;                    // ISO + SRT feed rate: 0 = as Zoom sends, 30, 60
};

inline bool isSlotFps(int fps) { return fps == 0 || fps == 30 || fps == 60; }

inline bool isZoomFeedHeight(int height) { return height == 1080 || height == 720 || height == 360; }
// The engine's subscribe "resolution": 0 = 360p, 1 = 720p, 2 = 1080p.
inline int engineResolutionForHeight(int height) { return height == 360 ? 0 : height == 720 ? 1 : 2; }
inline int heightForEngineResolution(int resolution) { return resolution == 0 ? 360 : resolution == 1 ? 720 : 1080; }

struct SubscriptionAction {
  enum class Kind { Subscribe, Unsubscribe };
  Kind kind = Kind::Subscribe;
  int slot = 0;
  std::uint32_t participantId = 0;
  std::string reason;  // "operator", "config", "rejoined", "left"
};

inline std::string slotSourceUuid(int slot) { return "edge-s" + std::to_string(slot); }

inline bool namesEqualIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

class SubscriptionBook {
 public:
  explicit SubscriptionBook(int maxSubscriptions = kDefaultMaxSubscriptions) { setCap(maxSubscriptions); }

  void setCap(int maxSubscriptions) {
    cap_ = std::clamp(maxSubscriptions, 1, kHardMaxSubscriptions);
  }
  [[nodiscard]] int cap() const { return cap_; }
  [[nodiscard]] const std::vector<Selection>& selections() const { return selections_; }
  void setConfigNames(std::vector<std::string> names) { configNames_ = std::move(names); }
  [[nodiscard]] const std::string& configWarning() const { return configWarning_; }

  [[nodiscard]] const Selection* findByParticipant(std::uint32_t id) const {
    for (const auto& s : selections_) if (s.present && s.participantId == id) return &s;
    return nullptr;
  }
  [[nodiscard]] const Selection* findBySlot(int slot) const {
    for (const auto& s : selections_) if (s.slot == slot) return &s;
    return nullptr;
  }

  // Operator subscribe/unsubscribe. Returns an empty string on success or the
  // refusal the operator sees. `out` receives the engine actions.
  //
  // `slot` 0 = lowest free (and a no-op if the guest is already routed).
  // A named slot is a crosspoint take (owner 2026-11-09): the guest goes to
  // THAT slot, MOVING from another slot if routed (their feed settings and
  // ISO arm travel with them) and REPLACING whoever held it. Recording guards
  // live in the controller, which knows which slots are being written.
  std::string request(const RosterEntry& who, bool on, std::vector<SubscriptionAction>& out,
                      const std::string& reason = "operator", int slot = 0, int height = 1080, int fps = 0) {
    if (who.id == 0) return "Unknown participant.";
    if (who.isMe) return "This Edge node cannot subscribe to itself.";
    if (on) {
      if (!isZoomFeedHeight(height)) return "Zoom feed resolution must be 1080, 720 or 360.";
      if (!isSlotFps(fps)) return "Frame rate must be 0 (as Zoom sends), 30 or 60.";
      if (slot != 0 && (slot < 1 || slot > cap_)) {
        return "Slot S" + std::to_string(slot) + " is outside this node's " + std::to_string(cap_) + " slots.";
      }
      const auto* existing = findByParticipant(who.id);
      const bool moving = existing != nullptr;
      if (existing && (slot == 0 || existing->slot == slot)) return {};  // already there: idempotent
      Selection s;
      s.height = height;
      s.fps = fps;
      if (existing) {  // move: the guest's settings travel with them
        s.height = existing->height;
        s.fps = existing->fps;
        s.iso = existing->iso;
        s.fromConfig = existing->fromConfig;
        const int from = existing->slot;
        out.push_back({SubscriptionAction::Kind::Unsubscribe, from, who.id, "moved"});
        eraseSlot(from);
      }
      if (slot != 0) {
        if (const auto* occupant = findBySlot(slot)) {  // replace
          if (occupant->present) out.push_back({SubscriptionAction::Kind::Unsubscribe, slot, occupant->participantId, "replaced"});
          eraseSlot(slot);
        }
      }
      if (static_cast<int>(selections_.size()) >= cap_) {
        return "This Edge node is at its limit of " + std::to_string(cap_) +
               " simultaneous subscriptions. Unsubscribe someone first, or add another Edge node.";
      }
      s.slot = slot != 0 ? slot : lowestFreeSlot();
      s.participantId = who.id;
      s.displayName = who.name;
      if (!moving) s.fromConfig = reason == "config";
      selections_.push_back(s);
      out.push_back({SubscriptionAction::Kind::Subscribe, s.slot, who.id, reason});
      return {};
    }
    // Off: also accepts a departed selection's slot via unsubscribeSlot().
    for (auto it = selections_.begin(); it != selections_.end(); ++it) {
      if (it->present && it->participantId == who.id) {
        out.push_back({SubscriptionAction::Kind::Unsubscribe, it->slot, it->participantId, reason});
        selections_.erase(it);
        return {};
      }
    }
    return {};  // not subscribed: idempotent
  }

  std::string unsubscribeSlot(int slot, std::vector<SubscriptionAction>& out) {
    for (auto it = selections_.begin(); it != selections_.end(); ++it) {
      if (it->slot == slot) {
        if (it->present) out.push_back({SubscriptionAction::Kind::Unsubscribe, slot, it->participantId, "operator"});
        selections_.erase(it);
        return {};
      }
    }
    return "No subscription in slot " + std::to_string(slot) + ".";
  }

  // Re-subscribe one slot at a new Zoom feed resolution.
  std::string setHeight(int slot, int height, std::vector<SubscriptionAction>& out) {
    if (!isZoomFeedHeight(height)) return "Zoom feed resolution must be 1080, 720 or 360.";
    for (auto& s : selections_) {
      if (s.slot != slot) continue;
      if (s.height == height) return {};
      s.height = height;
      if (s.present) {
        out.push_back({SubscriptionAction::Kind::Unsubscribe, slot, s.participantId, "resolution"});
        out.push_back({SubscriptionAction::Kind::Subscribe, slot, s.participantId, "resolution"});
      }
      return {};
    }
    return "No subscription in slot " + std::to_string(slot) + ".";
  }

  // The slot's output frame rate (ISO file + SRT feed). No engine action:
  // Zoom cannot be asked for a frame rate, only Edge's outputs can be paced.
  std::string setFps(int slot, int fps) {
    if (!isSlotFps(fps)) return "Frame rate must be 0 (as Zoom sends), 30 or 60.";
    for (auto& s : selections_) {
      if (s.slot == slot) { s.fps = fps; return {}; }
    }
    return "No subscription in slot " + std::to_string(slot) + ".";
  }

  std::string setIso(int slot, bool on) {
    for (auto& s : selections_) {
      if (s.slot == slot) { s.iso = on; return {}; }
    }
    return "No subscription in slot " + std::to_string(slot) + ".";
  }

  // Reconcile with a fresh, authoritative roster: mark departures, re-bind a
  // departed slot to a rejoined guest with the same display name, and pick up
  // config-listed names. Never adds anyone who is neither.
  void applyRoster(const std::vector<RosterEntry>& roster, std::vector<SubscriptionAction>& out) {
    auto inRoster = [&](std::uint32_t id) {
      return std::any_of(roster.begin(), roster.end(), [&](const RosterEntry& r) { return r.id == id; });
    };
    for (auto& s : selections_) {
      if (s.present && !inRoster(s.participantId)) {
        s.present = false;
        // The engine target stays; the slot keeps its regions for the rebind.
      }
    }
    // Back with the same Zoom id (a breakout room returning, a roster gap):
    // present again, and re-subscribed because the engine dropped it.
    for (auto& s : selections_) {
      if (!s.present && inRoster(s.participantId)) {
        s.present = true;
        out.push_back({SubscriptionAction::Kind::Subscribe, s.slot, s.participantId, "returned"});
      }
    }
    for (const auto& r : roster) {
      if (r.isMe || r.id == 0 || findByParticipant(r.id)) continue;
      for (auto& s : selections_) {
        if (!s.present && s.displayName == r.name && !r.name.empty()) {
          s.present = true;
          s.participantId = r.id;
          out.push_back({SubscriptionAction::Kind::Subscribe, s.slot, r.id, "rejoined"});
          break;
        }
      }
    }
    configWarning_.clear();
    for (const auto& wanted : configNames_) {
      if (wanted.empty()) continue;
      const bool alreadyHeld = std::any_of(selections_.begin(), selections_.end(), [&](const Selection& s) {
        return namesEqualIgnoreCase(s.displayName, wanted);
      });
      if (alreadyHeld) continue;
      const auto match = std::find_if(roster.begin(), roster.end(), [&](const RosterEntry& r) {
        return !r.isMe && namesEqualIgnoreCase(r.name, wanted);
      });
      if (match == roster.end()) continue;
      const auto refusal = request(*match, true, out, "config");
      if (!refusal.empty() && configWarning_.empty()) {
        configWarning_ = "Config lists \"" + wanted + "\" but all " + std::to_string(cap_) +
                         " subscription slots are in use.";
      }
    }
  }

  void clear() { selections_.clear(); configWarning_.clear(); }

 private:
  void eraseSlot(int slot) {
    selections_.erase(std::remove_if(selections_.begin(), selections_.end(),
                                     [&](const Selection& s) { return s.slot == slot; }),
                      selections_.end());
  }
  int lowestFreeSlot() const {
    for (int slot = 1; slot <= kHardMaxSubscriptions; ++slot) {
      if (!findBySlot(slot)) return slot;
    }
    return 0;
  }

  int cap_ = kDefaultMaxSubscriptions;
  std::vector<Selection> selections_;
  std::vector<std::string> configNames_;
  std::string configWarning_;
};

}  // namespace corevideo::edge
