#pragma once

// Node configuration, from an optional JSON file (see config/edge.example.json).
// Everything has a working default except the record folder, which is the
// operator's to choose: Edge never invents one.

#include <string>
#include <vector>

namespace corevideo::edge {

struct EdgeConfig {
  std::string displayName;           // shown to the meeting; default "CoreVideo Edge - <COMPUTERNAME>"
  std::string recordFolder;
  std::string filePrefix = "edge";
  int maxSubscriptions = 6;          // 1..8, see SubscriptionPolicy.h
  std::vector<std::string> subscribe;  // display names to subscribe when present (ISO on)
  int videoBitrateMbps = 18;
  bool hardwareEncode = true;
};

// Parses `text`. Unknown keys are ignored; a present key with the wrong type
// or an out-of-range value is an error (listed in `errors`), never a silent
// default.
EdgeConfig parseEdgeConfig(const std::string& text, std::vector<std::string>& errors);
EdgeConfig loadEdgeConfig(const std::string& path, std::vector<std::string>& errors);
std::string defaultDisplayName();

}  // namespace corevideo::edge
