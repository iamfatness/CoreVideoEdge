#include "edge/IsoSession.h"

#include "core/BoundedAsyncLog.h"
#include "edge/SteadyClock.h"
#include "modules/RecordingArtifactNaming.h"
#include "rpc/Json.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <future>

namespace corevideo::edge {
namespace {

using rpc::Json;
namespace naming = modules::recording;

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

std::string gb(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
  return text;
}

}  // namespace

std::string utcNowIso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_s(&tm, &seconds);
  char text[40];
  std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(millis));
  return text;
}

DiskEstimate estimateDisk(std::uint64_t freeBytes, size_t sources, const IsoWriterSettings& writer) {
  DiskEstimate out;
  out.freeBytes = freeBytes;
  const std::uint64_t bitsPerSecond =
      static_cast<std::uint64_t>(sources) *
      (static_cast<std::uint64_t>(std::max(1, writer.videoBitrateMbps)) * 1'000'000ull +
       static_cast<std::uint64_t>(std::max(1, writer.audioBitrateKbps)) * 1'000ull);
  out.bytesPerSecond = bitsPerSecond / 8;
  const std::uint64_t tenMinutes = out.bytesPerSecond * 600;
  const std::uint64_t hardMinimum = std::max<std::uint64_t>(2 * kGiB, tenMinutes);
  if (freeBytes < hardMinimum) {
    out.refusal = "Not enough disk space: " + gb(freeBytes) + " free. A 10-minute take of " +
                  std::to_string(sources) + " ISO(s) needs about " + gb(hardMinimum) + ".";
    return out;
  }
  if (out.bytesPerSecond > 0) {
    const auto minutes = freeBytes / out.bytesPerSecond / 60;
    if (minutes < 60) {
      out.warning = "Disk space for about " + std::to_string(minutes) + " minutes at this load (" + gb(freeBytes) +
                    " free).";
    }
  }
  return out;
}

std::string sessionFolderName(const std::string& prefix, const std::string& stamp) {
  return naming::sanitizeForFilename(prefix, "edge") + "-" + stamp;
}

std::string isoFileName(int index, const std::string& displayName, const std::string& sourceId) {
  char head[16];
  std::snprintf(head, sizeof(head), "ISO-%02d-", index);
  return std::string(head) + naming::sanitizeForFilename(displayName, sourceId) + ".mp4";
}

IsoSession::~IsoSession() {
  if (active_) stop();
}

std::string IsoSession::start(const IsoSessionRequest& request) {
  if (active_) return "Already recording.";
  if (request.sources.empty()) return "Turn on ISO for at least one subscribed source.";
  if (request.folder.empty()) return "Choose a record folder first.";
  std::error_code ec;
  const auto folder = std::filesystem::absolute(request.folder, ec);
  if (ec) return "The record folder path is not valid: " + request.folder.string();
  std::filesystem::create_directories(folder, ec);
  if (ec || !std::filesystem::is_directory(folder)) {
    return "Cannot use record folder " + folder.string() + ": " + (ec ? ec.message() : "not a directory") + ".";
  }
  const auto space = std::filesystem::space(folder, ec);
  if (ec) return "Cannot read free space for " + folder.string() + ": " + ec.message() + ".";
  const auto disk = estimateDisk(space.available, request.sources.size(), request.writer);
  if (!disk.refusal.empty()) return disk.refusal;

  auto dir = folder / sessionFolderName(request.prefix, naming::sessionTimestampFolder());
  for (int n = 2; std::filesystem::exists(dir) && n < 100; ++n) {
    dir = folder / (sessionFolderName(request.prefix, naming::sessionTimestampFolder()) + "-" + std::to_string(n));
  }
  if (!std::filesystem::create_directory(dir, ec) || ec) {
    return "Cannot create the session folder " + dir.string() + ": " + (ec ? ec.message() : "already exists") + ".";
  }
  {
    // Prove we can write here before announcing a take.
    const auto probe = dir / ".write-probe";
    std::ofstream out(probe, std::ios::binary);
    out << "ok";
    out.close();
    if (!out || !std::filesystem::exists(probe)) {
      std::filesystem::remove_all(dir, ec);
      return "The record folder is not writable: " + folder.string() + ".";
    }
    std::filesystem::remove(probe, ec);
  }

  request_ = request;
  request_.folder = folder;
  sessionDir_ = dir;
  startWarning_ = disk.warning;

  stoppedUtc_.clear();
  recorders_.clear();
  participantHistory_.clear();

  std::string failure;
  int index = 0;
  for (const auto& source : request_.sources) {
    ++index;
    IsoSourceConfig config;
    config.index = index;
    config.slot = source.slot;
    config.slotUuid = source.slotUuid;
    config.participantId = source.participantId;
    config.displayName = source.displayName;
    config.path = dir / isoFileName(index, source.displayName, "zoom" + std::to_string(source.participantId));
    config.shmPrefix = request_.shmPrefix;
    config.isoRingEpoch = request_.isoRingEpoch;

    config.writer = request_.writer;
    auto recorder = std::make_unique<IsoSourceRecorder>();
    std::string error;
    if (!recorder->start(config, error)) {
      failure = error;
      break;
    }
    recorders_.push_back(std::move(recorder));
    participantHistory_.push_back({source.participantId});
  }
  if (!failure.empty()) {
    // All or nothing: a take that silently lacks one requested ISO is the
    // failure this whole design exists to prevent.
    const auto now = steadyNow100ns();
    for (auto& recorder : recorders_) recorder->stop(now);
    recorders_.clear();
    std::filesystem::remove_all(dir, ec);
    sessionDir_.clear();
    return failure;
  }
  // ONE epoch for every file in the take (Pro: one RecordingPtsClock epoch
  // across all ISO writers), so a clap lands at the same timestamp everywhere.
  // Taken after every file is open: no file's head waits on another's setup.
  epoch100ns_ = steadyNow100ns();
  startedUtc_ = utcNowIso8601();
  for (auto& recorder : recorders_) recorder->begin(epoch100ns_);
  active_ = true;
  writeManifest(false);
  core::nativeLogf("[iso] take started: %s (%zu ISO)\n", dir.string().c_str(), recorders_.size());
  return {};
}

void IsoSession::stop() {
  if (!active_) return;
  const auto stopAt = steadyNow100ns();
  std::vector<std::future<void>> stopping;
  for (auto& recorder : recorders_) {
    stopping.push_back(std::async(std::launch::async, [&recorder, stopAt] { recorder->stop(stopAt); }));
  }
  for (auto& f : stopping) f.wait();
  stoppedUtc_ = utcNowIso8601();
  writeManifest(true);
  active_ = false;
  core::nativeLogf("[iso] take stopped: %s\n", sessionDir_.string().c_str());
}

std::vector<IsoSourceHealth> IsoSession::health() const {
  std::vector<IsoSourceHealth> out;
  for (const auto& recorder : recorders_) out.push_back(recorder->health());
  return out;
}

std::string IsoSession::diskWarning() const {
  if (!manifestError_.empty()) return manifestError_;
  if (!active_) return {};
  std::error_code ec;
  const auto space = std::filesystem::space(sessionDir_, ec);
  if (ec) return "Cannot read free space on the record volume: " + ec.message() + ".";
  if (space.available < kGiB) return "Disk nearly full: " + gb(space.available) + " left on the record volume.";
  return startWarning_;
}

void IsoSession::rebind(int slot, std::uint32_t participantId) {
  for (size_t i = 0; i < recorders_.size(); ++i) {
    if (request_.sources[i].slot != slot) continue;
    recorders_[i]->setParticipant(participantId);
    participantHistory_[i].push_back(participantId);
  }
  if (active_) writeManifest(false);
}

void IsoSession::writeManifest(bool final) {
  Json::Array isos;
  const auto healths = health();
  for (size_t i = 0; i < healths.size(); ++i) {
    const auto& h = healths[i];
    Json::Array ids;
    for (auto id : participantHistory_[i]) ids.emplace_back(static_cast<double>(id));
    Json::Object entry{
        {"index", h.index},
        {"slot", request_.sources[i].slotUuid},
        {"sourceId", "zoom:" + std::to_string(participantHistory_[i].front())},
        {"kind", "zoom-participant"},
        {"name", h.displayName},
        {"file", std::filesystem::path(h.path).filename().string()},
        {"zoomUserIds", Json(std::move(ids))},
        {"state", h.state},
    };
    if (final) {
      entry.emplace("videoFrames", static_cast<double>(h.videoFrames));
      entry.emplace("prerollFrames", static_cast<double>(h.prerollFrames));
      entry.emplace("videoLost", static_cast<double>(h.videoLost));
      entry.emplace("audioSamples", static_cast<double>(h.audioSamples));
      entry.emplace("silenceSamples", static_cast<double>(h.silenceSamples));
      entry.emplace("audioLostPackets", static_cast<double>(h.audioLostPackets));
      entry.emplace("bytes", static_cast<double>(h.bytes));
    }
    if (!h.encoder.empty()) entry.emplace("encoder", h.encoder);
    if (!h.warning.empty()) entry.emplace("warning", h.warning);
    isos.emplace_back(std::move(entry));
  }
  Json::Object manifest{
      {"product", "CoreVideo Edge"},
      {"version", EDGE_VERSION},
      {"session", sessionDir_.filename().string()},
      {"node", request_.nodeName},
      {"meetingId", request_.meetingId},
      {"startedUtc", startedUtc_},
      {"complete", final},
      {"epoch", Json::Object{
          {"note", "Every file's t=0 is this instant. steady100ns is the Windows QPC-based steady clock of this node; "
                   "startedUtc is the same instant on the wall clock, for aligning takes from several nodes."},
          {"steady100ns", static_cast<double>(epoch100ns_)},
      }},
      {"video", Json::Object{
          {"codec", "h264"},
          {"width", request_.writer.width},
          {"height", request_.writer.height},
          {"frameRate", "variable (source timing)"},
          {"bitrateMbps", request_.writer.videoBitrateMbps},
          {"range", "bt709-full"},
      }},
      {"audio", Json::Object{
          {"codec", "aac"},
          {"sampleRate", request_.writer.audioSampleRate},
          {"channels", 2},
          {"bitrateKbps", request_.writer.audioBitrateKbps},
          {"content", "isolated participant audio (pre-mix), silence-filled to the epoch"},
      }},
      {"isos", Json(std::move(isos))},
  };
  if (final) manifest.emplace("stoppedUtc", stoppedUtc_);
  const auto path = sessionDir_ / "manifest.json";
  const auto temp = sessionDir_ / "manifest.json.tmp";
  std::ofstream out(temp, std::ios::binary | std::ios::trunc);
  out << Json(std::move(manifest)).stringify() << "\n";
  out.close();
  std::error_code ec;
  if (out) std::filesystem::rename(temp, path, ec);
  if (!out || ec) {
    manifestError_ = "Could not write " + path.string() + (ec ? ": " + ec.message() : std::string()) + ".";
    core::nativeLogf("[iso] %s\n", manifestError_.c_str());
  } else {
    manifestError_.clear();
  }
}

}  // namespace corevideo::edge
