// corevideo-edge: headless CoreVideo Edge node.
//
//   corevideo-edge.exe [--config <edge.json>] [--engine <path> | --fake]
//
// stdin : JSON-line commands      (docs/PROTOCOL.md)
// stdout: JSON-line state/results (docs/PROTOCOL.md) - nothing else is written here
// stderr: diagnostics log
//
// The operator app runs this as a child process. Scripts and tests drive it the
// same way (scripts/validate-edge-iso.mjs).

#include "core/BoundedAsyncLog.h"
#include "edge/EdgeConfig.h"
#include "edge/EdgeController.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::filesystem::path exeDir() {
  wchar_t buffer[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  return std::filesystem::path(std::wstring(buffer, n)).parent_path();
}

void writeStdout(const std::string& line) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  const std::string out = line + "\n";
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
}

void usage() {
  std::cerr << "usage: corevideo-edge [--config <edge.json>] [--engine <path> | --fake] [--version]\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace corevideo::edge;
  EdgeOptions options;
  const auto dir = exeDir();
  options.enginePath = (dir / "corevideo-zoom-engine.exe").string();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      options.configPath = argv[++i];
    } else if (arg == "--engine" && i + 1 < argc) {
      options.enginePath = argv[++i];
    } else if (arg == "--fake") {
      options.fakeEngine = true;
      options.enginePath = (dir / "corevideo-zoom-engine-fake.exe").string();
    } else if (arg == "--version") {
      std::cout << "corevideo-edge " << EDGE_VERSION << "\n";
      return 0;
    } else {
      usage();
      return 2;
    }
  }
  if (!options.configPath.empty()) {
    options.config = loadEdgeConfig(options.configPath, options.configErrors);
  } else {
    options.config.displayName = defaultDisplayName();
  }
  for (const auto& error : options.configErrors) corevideo::core::nativeLogf("[config] %s\n", error.c_str());
  corevideo::core::nativeLogf("[edge] corevideo-edge %s engine=%s\n", EDGE_VERSION, options.enginePath.c_str());

  EdgeController controller(std::move(options), writeStdout);
  std::thread input([&controller] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) controller.postCommand(std::move(line));
    }
    controller.postEndOfInput();
  });
  input.detach();  // blocked in a read on a pipe the parent owns; exiting closes it
  return controller.run();
}
