#include "edge/EngineSupervisor.h"

#include "core/BoundedAsyncLog.h"
#include "rpc/Json.h"
#include "engine-ipc.h"

#include <windows.h>

#include <chrono>

namespace corevideo::edge {

EngineSupervisor::~EngineSupervisor() { stop(); }

bool EngineSupervisor::start(const std::string& executablePath, Sink sink, std::string& error) {
  stop();
  // The fake engine streams EVERY participant unless told not to. Edge never
  // pulls anyone it was not asked to (owner rule), so this is forced off for
  // every child. The real engine ignores the variable.
  SetEnvironmentVariableA("COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE", "0");

  client_ = std::make_unique<modules::ZoomEngineProcessClient>();
  modules::ZoomEngineProcessOptions options;
  options.executablePath = executablePath;
  options.connectTimeoutMs = 30000;
  if (!client_->start(options)) {
    error = "Could not start the Zoom engine (" + executablePath + "): " + client_->lastError();
    client_.reset();
    return false;
  }
  const auto generation = ++generation_;
  stopping_ = false;
  reader_ = std::thread([this, sink = std::move(sink), generation]() {
    auto* client = client_.get();
    while (true) {
      auto line = client->readLine();
      if (!line) {
        EngineNotice notice;
        notice.kind = EngineNotice::Kind::Exited;
        notice.generation = generation;
        notice.reason = stopping_.load() ? "stopped" : client->lastError();
        sink(std::move(notice));
        return;
      }
      if (line->empty()) continue;
      auto event = modules::parseZoomEngineEvent(*line);
      if (!event) continue;  // parseZoomEngineEvent already logged it
      EngineNotice notice;
      notice.generation = generation;
      notice.event = std::move(*event);
      // The engine forwards every SDK meeting status as a debug line; Edge needs
      // the number to tell a breakout-room move (14/15) or a reconnect (5) from
      // a real leave. The lifted parser keeps only the stage.
      if (notice.event.kind == modules::ZoomEngineEventKind::Debug && notice.event.stage == "meeting_status") {
        if (auto json = rpc::Json::parse(*line)) {
          if (const auto* status = json->get("status"); status && status->isNumber())
            notice.meetingStatus = static_cast<int>(status->asNumber());
        }
      }
      sink(std::move(notice));
    }
  });
  core::nativeLogf("[engine] started %s (token %s)\n", executablePath.c_str(), client_->instanceToken().c_str());
  return true;
}

bool EngineSupervisor::send(const std::string& line) {
  return client_ && client_->sendLine(line);
}

bool EngineSupervisor::running() const { return client_ && client_->running(); }

std::string EngineSupervisor::shmPrefix() const {
  return client_ ? ipc_shm_prefix(client_->instanceToken()) : std::string();
}

void EngineSupervisor::stop() {
  if (!client_) return;
  stopping_ = true;
  // Order (Pro's ZoomEngineRuntime teardown): ask nicely, let the engine leave
  // and exit (its exit breaks the pipe and ends our blocked read), and only
  // then force it. Never join a reader that might be blocked forever.
  (void)client_->sendLine(modules::buildZoomEngineQuitCommand());
  if (reader_.joinable()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    HANDLE handle = reader_.native_handle();
    while (WaitForSingleObject(handle, 50) == WAIT_TIMEOUT) {
      if (std::chrono::steady_clock::now() >= deadline) {
        core::nativeLogf("[engine] did not exit after quit; cancelling the read and terminating\n");
        CancelSynchronousIo(handle);
        client_->terminate();
        break;
      }
    }
    reader_.join();
  }
  client_->stop();
  client_.reset();
}

}  // namespace corevideo::edge
