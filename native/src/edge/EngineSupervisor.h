#pragma once

// Owns one corevideo-zoom-engine (or the fake) child process: spawn, the
// JSON-line pipes, and a reader thread that hands every parsed event to the
// controller. Spawning and parsing are CoreVideo Pro's
// (ZoomEngineProcessClient / parseZoomEngineEvent); this adds lifetime only.

#include "modules/ZoomEngineClient.h"
#include "modules/ZoomEngineProcess.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace corevideo::edge {

struct EngineNotice {
  enum class Kind { Event, Exited };
  Kind kind = Kind::Event;
  std::uint64_t generation = 0;  // which spawn produced this notice
  modules::ZoomEngineEvent event;
  std::string reason;            // Exited: why the pipe closed
};

class EngineSupervisor {
 public:
  using Sink = std::function<void(EngineNotice)>;

  EngineSupervisor() = default;
  ~EngineSupervisor();
  EngineSupervisor(const EngineSupervisor&) = delete;
  EngineSupervisor& operator=(const EngineSupervisor&) = delete;

  bool start(const std::string& executablePath, Sink sink, std::string& error);
  bool send(const std::string& line);
  // Bounded: asks the engine to quit, then cancels the blocked read and kills
  // the process if it has not gone within a few seconds.
  void stop();

  [[nodiscard]] bool running() const;
  [[nodiscard]] std::uint64_t generation() const { return generation_; }
  [[nodiscard]] std::string shmPrefix() const;  // per-instance SHM name prefix

 private:
  std::unique_ptr<modules::ZoomEngineProcessClient> client_;
  std::thread reader_;
  std::atomic<bool> stopping_{false};
  std::uint64_t generation_ = 0;
};

}  // namespace corevideo::edge
