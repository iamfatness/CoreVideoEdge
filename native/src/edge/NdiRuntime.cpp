#include "edge/NdiRuntime.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace corevideo::edge::ndi {

namespace {

#if defined(_WIN32)
using Library = HMODULE;
Library open(const std::filesystem::path& p) { return LoadLibraryW(p.wstring().c_str()); }
void* sym(Library l, const char* n) { return reinterpret_cast<void*>(GetProcAddress(l, n)); }
#else
using Library = void*;
Library open(const std::filesystem::path& p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* sym(Library l, const char* n) { return dlsym(l, n); }
#endif

std::vector<std::filesystem::path> candidates() {
  std::vector<std::filesystem::path> list;
#if defined(_WIN32)
  if (const char* root = std::getenv("PROGRAMFILES")) {
    const std::filesystem::path base(root);
    list.push_back(base / "NDI/NDI 6 Runtime/v6/Bin/x64/Processing.NDI.Lib.x64.dll");
    list.push_back(base / "NDI/NDI 6 Tools/Runtime/Processing.NDI.Lib.x64.dll");
    list.push_back(base / "NDI/NDI 5 Runtime/v5/Bin/x64/Processing.NDI.Lib.x64.dll");
  }
  if (const char* root = std::getenv("NDI_RUNTIME_DIR")) list.push_back(std::filesystem::path(root) / "Processing.NDI.Lib.x64.dll");
  list.emplace_back("Processing.NDI.Lib.x64.dll");  // last: a search-path lookup
#elif defined(__APPLE__)
  // The NDI installers export NDI_RUNTIME_DIR_V6 / _V5; the SDK and Tools
  // install libndi.dylib in these folders.
  for (const char* name : {"NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5", "NDI_RUNTIME_DIR"}) {
    if (const char* root = std::getenv(name); root && *root) list.push_back(std::filesystem::path(root) / "libndi.dylib");
  }
  list.emplace_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
  list.emplace_back("/usr/local/lib/libndi.dylib");
  list.emplace_back("/opt/homebrew/lib/libndi.dylib");
  list.emplace_back("libndi.dylib");  // last: the dyld search path
#else
  list.emplace_back("libndi.so.6");
  list.emplace_back("libndi.so.5");
  list.emplace_back("libndi.so");
#endif
  return list;
}

template <typename T>
void bind(T& fn, Library l, const char* name) {
  fn = reinterpret_cast<T>(sym(l, name));
}

}  // namespace

const Api* load(std::string& error) {
  static std::once_flag once;
  static Api api;
  static bool ok = false;
  static std::string loadError;
  std::call_once(once, [] {
    Library lib = nullptr;
    for (const auto& c : candidates()) {
      if ((lib = open(c))) break;
    }
    if (!lib) {
#if defined(__APPLE__)
      loadError = "The NDI runtime is not installed (install NDI Tools or the NDI SDK for Apple, or set NDI_RUNTIME_DIR_V6).";
#else
      loadError = "The NDI runtime is not installed (install NDI 6 Runtime or NDI Tools, or set NDI_RUNTIME_DIR).";
#endif
      return;
    }
    bind(api.initialize, lib, "NDIlib_initialize");
    bind(api.sendCreate, lib, "NDIlib_send_create");
    bind(api.sendDestroy, lib, "NDIlib_send_destroy");
    bind(api.sendVideo, lib, "NDIlib_send_send_video_v2");
    bind(api.sendAudio, lib, "NDIlib_send_send_audio_v2");
    bind(api.sendConnections, lib, "NDIlib_send_get_no_connections");
    bind(api.findCreate, lib, "NDIlib_find_create_v2");
    bind(api.findDestroy, lib, "NDIlib_find_destroy");
    bind(api.findWait, lib, "NDIlib_find_wait_for_sources");
    bind(api.findSources, lib, "NDIlib_find_get_current_sources");
    bind(api.recvCreate, lib, "NDIlib_recv_create_v3");
    bind(api.recvDestroy, lib, "NDIlib_recv_destroy");
    bind(api.recvCapture, lib, "NDIlib_recv_capture_v2");
    bind(api.freeVideo, lib, "NDIlib_recv_free_video_v2");
    bind(api.freeAudio, lib, "NDIlib_recv_free_audio_v2");
    if (!api.canSend()) {
      loadError = "The installed NDI runtime is missing the send API this build needs.";
      return;
    }
    if (!api.initialize()) {
      loadError = "The NDI runtime refused to initialise (unsupported CPU?).";
      return;
    }
    ok = true;
  });
  if (!ok) {
    error = loadError;
    return nullptr;
  }
  return &api;
}

}  // namespace corevideo::edge::ndi
