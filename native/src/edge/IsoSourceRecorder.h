#pragma once

// Records ONE subscribed participant to ONE ISO MP4: that participant's video
// (from the engine's per-source ISO arrival ring, every frame, not the
// latest-wins preview region) and that participant's isolated, pre-mix audio
// (from the per-source audio ring), both on the take's shared epoch.
//
// Owns one thread. All encoder work (Media Foundation / AVFoundation) happens on it.

#include "edge/IsoMp4Writer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace corevideo::edge {

struct IsoSourceConfig {
  int index = 0;                 // NN in ISO-NN-<name>.mp4
  int slot = 0;
  std::string slotUuid;          // engine source_uuid ("edge-s1")
  std::uint32_t participantId = 0;
  std::string displayName;
  std::filesystem::path path;
  std::string shmPrefix;         // ipc_shm_prefix(engine instance token)
  std::uint32_t isoRingEpoch = 0;  // value sent with the engine's iso_video command
  IsoWriterSettings writer;
};

struct IsoSourceHealth {
  int index = 0;
  int slot = 0;
  std::string displayName;
  std::uint32_t participantId = 0;
  std::string path;
  std::string state;            // opening | recording | finalizing | finalized | failed
  std::string encoder;          // measured: hardware | software | ""
  std::int64_t videoFrames = 0;       // real frames written
  std::int64_t prerollFrames = 0;
  std::int64_t heldRepeats = 0;       // held frame re-queued while the camera stalled (not frames)
  std::int64_t videoDropped = 0;      // refused because the encoder backlog was full (a defect if > 0)
  std::int64_t encoderBacklogPeak = 0;  // most samples the Sink Writer ever held unwritten
  std::int64_t cadenceRepeats = 0;    // constant-rate mode: grid slots filled by repeating a frame
  std::int64_t cadenceSkipped = 0;    // constant-rate mode: arrivals replaced within one grid slot
  std::int64_t videoLost = 0;        // arrivals the ring overwrote/tore before we read them
  std::int64_t audioSamples = 0;      // real (non-silence) samples written, per channel
  std::int64_t silenceSamples = 0;    // silence-fill written, per channel
  std::int64_t audioLostPackets = 0;
  std::uint32_t lastWidth = 0;        // last frame actually received from Zoom
  std::uint32_t lastHeight = 0;
  std::int64_t msSinceVideo = -1;     // -1: none received yet this take
  std::int64_t msSinceAudio = -1;
  std::int64_t bytes = 0;
  std::string warning;                // sticky: first writer/format failure
};

class IsoSourceRecorder {
 public:
  IsoSourceRecorder() = default;
  ~IsoSourceRecorder();
  IsoSourceRecorder(const IsoSourceRecorder&) = delete;
  IsoSourceRecorder& operator=(const IsoSourceRecorder&) = delete;

  // Opens the file on the recorder thread and waits for the result, so a bad
  // path or a refused encoder is reported here, before the take is announced.
  // Nothing is read until begin().
  bool start(const IsoSourceConfig& config, std::string& error);
  // Sets the shared epoch and starts reading. Called once every file of the
  // take has opened, so no file's head waits on another file's encoder setup.
  void begin(std::int64_t epoch100ns);
  // Bring the files to `stop100ns` (steady clock), finalize, and join.
  void stop(std::int64_t stop100ns);
  void setParticipant(std::uint32_t participantId);
  [[nodiscard]] IsoSourceHealth health() const;

 private:
  void run();
  void setWarning(const std::string& text);

  IsoSourceConfig config_;
  std::thread thread_;
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> begun_{false};
  std::atomic<std::int64_t> epoch100ns_{0};
  std::atomic<std::int64_t> stopAt100ns_{0};

  std::mutex openMutex_;
  bool openDone_ = false;
  bool openOk_ = false;
  std::string openError_;
  std::condition_variable_any openCv_;

  mutable std::mutex healthMutex_;
  IsoSourceHealth health_;
  std::atomic<std::uint32_t> participantId_{0};
};

}  // namespace corevideo::edge
