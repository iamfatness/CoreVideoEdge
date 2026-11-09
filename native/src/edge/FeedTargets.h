#pragma once

// Where an Edge slot's live feed goes in CoreVideo Pro. Pure.
//
// Identities are per node + slot, never per guest: Pro keys an NDI input by a
// hash of the source name and an SRT input by its listener port, so a slot
// keeps its Pro wiring when the guest in it changes or rejoins.

#include <cctype>
#include <string>

namespace corevideo::edge {

enum class FeedMode { Off, Ndi, Srt };

inline const char* feedModeName(FeedMode m) {
  return m == FeedMode::Ndi ? "ndi" : m == FeedMode::Srt ? "srt" : "off";
}

inline bool parseFeedMode(const std::string& text, FeedMode& out) {
  if (text == "off") out = FeedMode::Off;
  else if (text == "ndi") out = FeedMode::Ndi;
  else if (text == "srt") out = FeedMode::Srt;
  else return false;
  return true;
}

struct SrtSettings {
  std::string host;           // Pro's machine
  int basePort = 10000;       // Pro's default: SRT input n listens on 10000 + n - 1
  int latencyMs = 120;        // Pro's default
  std::string passphrase;     // empty = unencrypted; otherwise 10..79 chars (SRT rule)
};

// "CoreVideo Edge EDGE-01 S3". The node name is the display name without the
// "CoreVideo Edge - " prefix when present.
inline std::string ndiFeedName(const std::string& nodeName, int slot) {
  // Drop the product name in any form ("CoreVideo Edge - X", "CoreVideo Edge (X)").
  std::string node = nodeName;
  const std::string product = "CoreVideo Edge";
  if (node.rfind(product, 0) == 0) node = node.substr(product.size());
  const auto isSep = [](char c) { return c == ' ' || c == '-' || c == '(' || c == ')' || c == ':'; };
  while (!node.empty() && isSep(node.front())) node.erase(node.begin());
  while (!node.empty() && isSep(node.back())) node.pop_back();
  std::string clean;
  for (const char c : node) clean += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') ? c : '-';
  if (clean.empty()) clean = "node";
  return "CoreVideo Edge " + clean + " S" + std::to_string(slot);
}

inline int srtPort(const SrtSettings& s, int slot) { return s.basePort + slot - 1; }

// Empty when valid, else the reason (operator-readable).
inline std::string validateSrt(const SrtSettings& s) {
  if (s.host.empty()) return "Set Pro's address (the machine running CoreVideo Pro).";
  if (s.basePort < 1024 || s.basePort + 7 > 65535) return "SRT base port must be 1024..65528.";
  if (s.latencyMs < 20 || s.latencyMs > 8000) return "SRT latency must be 20..8000 ms.";
  if (!s.passphrase.empty() && (s.passphrase.size() < 10 || s.passphrase.size() > 79))
    return "SRT passphrase must be 10 to 79 characters (or empty for none).";
  return {};
}

// srt://host:port?mode=caller&transtype=live&latency=<us>[&passphrase=..&pbkeylen=16]
inline std::string srtUrl(const SrtSettings& s, int slot, bool redact) {
  std::string url = "srt://" + s.host + ":" + std::to_string(srtPort(s, slot)) +
                    "?mode=caller&transtype=live&latency=" + std::to_string(s.latencyMs * 1000);
  if (!s.passphrase.empty()) url += "&passphrase=" + (redact ? std::string("<redacted>") : s.passphrase) + "&pbkeylen=16";
  return url;
}

}  // namespace corevideo::edge
