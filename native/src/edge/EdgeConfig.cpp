#include "edge/EdgeConfig.h"

#include "edge/SubscriptionPolicy.h"
#include "rpc/Json.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace corevideo::edge {

std::string defaultDisplayName() {
  const char* host = std::getenv("COMPUTERNAME");
  return host && *host ? std::string("CoreVideo Edge - ") + host : std::string("CoreVideo Edge");
}

EdgeConfig parseEdgeConfig(const std::string& text, std::vector<std::string>& errors) {
  EdgeConfig config;
  config.displayName = defaultDisplayName();
  std::string parseError;
  const auto parsed = rpc::Json::parse(text, &parseError);
  if (!parsed || !parsed->isObject()) {
    errors.push_back("config is not a JSON object" + (parseError.empty() ? std::string() : ": " + parseError));
    return config;
  }
  const auto& root = *parsed;
  const auto readString = [&](const char* key, std::string& target) {
    if (const auto* value = root.get(key)) {
      if (value->isString()) target = value->asString();
      else errors.push_back(std::string(key) + " must be a string");
    }
  };
  const auto readInt = [&](const char* key, int& target, int lo, int hi) {
    if (const auto* value = root.get(key)) {
      const double n = value->asNumber(-1);
      if (!value->isNumber() || n != static_cast<int>(n) || n < lo || n > hi) {
        errors.push_back(std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " +
                         std::to_string(hi));
      } else {
        target = static_cast<int>(n);
      }
    }
  };
  readString("displayName", config.displayName);
  if (config.displayName.empty()) config.displayName = defaultDisplayName();
  readString("recordFolder", config.recordFolder);
  readString("filePrefix", config.filePrefix);
  readInt("maxSubscriptions", config.maxSubscriptions, 1, kHardMaxSubscriptions);
  readInt("videoBitrateMbps", config.videoBitrateMbps, 4, 50);
  if (const auto* value = root.get("hardwareEncode")) {
    if (value->isBool()) config.hardwareEncode = value->asBool();
    else errors.push_back("hardwareEncode must be true or false");
  }
  if (const auto* value = root.get("subscribe")) {
    if (!value->isArray()) {
      errors.push_back("subscribe must be a list of participant display names");
    } else {
      for (const auto& name : value->asArray()) {
        if (name.isString() && !name.asString().empty()) config.subscribe.push_back(name.asString());
        else errors.push_back("subscribe entries must be non-empty strings");
      }
    }
  }
  return config;
}

EdgeConfig loadEdgeConfig(const std::string& path, std::vector<std::string>& errors) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    errors.push_back("cannot read config file " + path);
    EdgeConfig config;
    config.displayName = defaultDisplayName();
    return config;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parseEdgeConfig(text.str(), errors);
}

}  // namespace corevideo::edge
