#pragma once

// Which meeting participants this Edge node pulls from Zoom, and nothing else.
//
// Product rule (owner, 2026-11-08): an Edge exists to spend its OWN Zoom
// downlink budget on the guests CoreVideo Pro is not pulling. So:
//   - never auto-subscribe the meeting; only explicit operator picks and names
//     listed in the node's config file are subscribed;
//   - a hard cap (default 6 simultaneous 1080p subscriptions) is enforced here,
//     not in the UI, so no caller can exceed it;
//   - resolution is never traded for count: every subscription asks for 1080p.
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

inline constexpr int kDefaultMaxSubscriptions = 6;
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
};

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
  std::string request(const RosterEntry& who, bool on, std::vector<SubscriptionAction>& out,
                      const std::string& reason = "operator") {
    if (who.id == 0) return "Unknown participant.";
    if (who.isMe) return "This Edge node cannot subscribe to itself.";
    if (on) {
      if (findByParticipant(who.id)) return {};  // already subscribed: idempotent
      if (static_cast<int>(selections_.size()) >= cap_) {
        return "This Edge node is at its limit of " + std::to_string(cap_) +
               " simultaneous 1080p subscriptions. Unsubscribe someone first, or add another Edge node.";
      }
      Selection s;
      s.slot = lowestFreeSlot();
      s.participantId = who.id;
      s.displayName = who.name;
      s.fromConfig = reason == "config";
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
