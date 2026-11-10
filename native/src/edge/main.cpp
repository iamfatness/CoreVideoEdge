// corevideo-edge: headless CoreVideo Edge node.
//
//   corevideo-edge[.exe] [--config <edge.json>] [--engine <path> | --fake]
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

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::filesystem::path exeDir() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  return std::filesystem::path(std::wstring(buffer, n)).parent_path();
#elif defined(__APPLE__)
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) return std::filesystem::current_path();
  std::error_code ec;
  auto path = std::filesystem::canonical(buffer, ec);
  return (ec ? std::filesystem::path(buffer) : path).parent_path();
#else
  std::error_code ec;
  return std::filesystem::canonical("/proc/self/exe", ec).parent_path();
#endif
}

// The engine and fake engine next to this node. Windows: <dir>/<name>.exe.
// macOS (contract with the app packager): the real engine is a bundle, because
// the Zoom SDK only authenticates from <App>.app/Contents/MacOS with the SDK in
// Contents/Frameworks. Shipped: CoreVideo Edge.app/Contents/MacOS/corevideo-edge
// next to Contents/Helpers/corevideo-zoom-engine.app; build tree: the .app next
// to the node. The fake is a plain binary next to the node.
std::filesystem::path defaultEnginePath(const std::filesystem::path& dir) {
#if defined(_WIN32)
  return dir / "corevideo-zoom-engine.exe";
#elif defined(__APPLE__)
  const auto inBundle = dir / "corevideo-zoom-engine.app" / "Contents" / "MacOS" / "corevideo-zoom-engine";
  const auto helpers = dir.parent_path() / "Helpers" / inBundle;
  std::error_code ec;
  if (std::filesystem::exists(helpers, ec)) return helpers;
  return dir / inBundle;
#else
  return dir / "corevideo-zoom-engine";
#endif
}

std::filesystem::path fakeEnginePath(const std::filesystem::path& dir) {
#if defined(_WIN32)
  return dir / "corevideo-zoom-engine-fake.exe";
#else
  return dir / "corevideo-zoom-engine-fake";
#endif
}

void writeStdout(const std::string& line) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  const std::string out = line + "\n";
#if defined(_WIN32)
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
#else
  size_t offset = 0;
  while (offset < out.size()) {
    const auto n = ::write(STDOUT_FILENO, out.data() + offset, out.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // the app went away; nothing to tell
    offset += static_cast<size_t>(n);
  }
#endif
}

void usage() {
  std::cerr << "usage: corevideo-edge [--config <edge.json>] [--engine <path> | --fake] [--version]\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace corevideo::edge;
  EdgeOptions options;
  const auto dir = exeDir();
#if !defined(_WIN32)
  // A closed pipe (the app, an engine socket, an FFmpeg stdin) is a write
  // error to handle, never a reason for the node to die. FFmpeg children get
  // the default disposition back (posix_spawn SETSIGDEF).
  std::signal(SIGPIPE, SIG_IGN);
#endif
  options.enginePath = defaultEnginePath(dir).string();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      options.configPath = argv[++i];
    } else if (arg == "--engine" && i + 1 < argc) {
      options.enginePath = argv[++i];
    } else if (arg == "--fake") {
      options.fakeEngine = true;
      options.enginePath = fakeEnginePath(dir).string();
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
