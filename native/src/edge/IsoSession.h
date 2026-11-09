#pragma once

// One ISO take: a session folder, one IsoSourceRecorder per selected source,
// one shared epoch, and manifest.json.
//
//   <RecordingFolder>/<prefix>-<yyyymmdd-hhmmss>/
//     ISO-01-<SafeName>.mp4
//     ISO-02-<SafeName>.mp4
//     manifest.json
//
// (CoreVideo Pro's layout, docs/iso-record-spec.md section 5, minus
// Program.mp4: an Edge node has no program.)
//
// Loud failures, never a silent fallback: an empty, uncreatable or unwritable
// folder, or a volume without room for a short take, refuses to start. There is
// no %TEMP% path anywhere in Edge.

#include "edge/IsoSourceRecorder.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::edge {

struct IsoSourceRequest {
  int slot = 0;
  std::string slotUuid;
  std::uint32_t participantId = 0;
  std::string displayName;
};

struct IsoSessionRequest {
  std::filesystem::path folder;
  std::string prefix = "edge";
  std::string meetingId;
  std::string nodeName;
  std::string shmPrefix;
  std::uint32_t isoRingEpoch = 0;
  IsoWriterSettings writer;
  std::vector<IsoSourceRequest> sources;  // in ISO-NN order
};

struct DiskEstimate {
  std::uint64_t freeBytes = 0;
  std::uint64_t bytesPerSecond = 0;
  std::string refusal;  // non-empty: do not start
  std::string warning;  // non-empty: start, but say this
};

// Pure: the space rule. Refuse when the volume cannot hold 10 minutes of this
// load (and never below 2 GB free); warn under 60 minutes.
DiskEstimate estimateDisk(std::uint64_t freeBytes, size_t sources, const IsoWriterSettings& writer);

// Pure: "<prefix>-<stamp>" with the prefix made filename-safe.
std::string sessionFolderName(const std::string& prefix, const std::string& stamp);
// Pure: "ISO-01-<SafeName>.mp4".
std::string isoFileName(int index, const std::string& displayName, const std::string& sourceId);

class IsoSession {
 public:
  IsoSession() = default;
  ~IsoSession();

  // Returns an empty string on success, else the refusal the operator sees.
  std::string start(const IsoSessionRequest& request);
  // Finalizes every file (in parallel) and rewrites manifest.json.
  void stop();

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] const std::filesystem::path& sessionDir() const { return sessionDir_; }
  [[nodiscard]] std::int64_t epoch100ns() const { return epoch100ns_; }
  [[nodiscard]] const std::string& startedUtc() const { return startedUtc_; }
  [[nodiscard]] std::vector<IsoSourceHealth> health() const;
  [[nodiscard]] std::string diskWarning() const;  // live check, call at a low rate
  void rebind(int slot, std::uint32_t participantId);

 private:
  void writeManifest(bool final);

  IsoSessionRequest request_;
  std::filesystem::path sessionDir_;
  std::int64_t epoch100ns_ = 0;
  std::string startedUtc_;
  std::string stoppedUtc_;
  std::string startWarning_;
  std::string manifestError_;
  std::vector<std::unique_ptr<IsoSourceRecorder>> recorders_;
  std::vector<std::vector<std::uint32_t>> participantHistory_;
  bool active_ = false;
};

std::string utcNowIso8601();

}  // namespace corevideo::edge
