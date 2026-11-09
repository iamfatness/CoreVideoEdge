// corevideo-edge-ndi-probe: verification tool for Edge's NDI feeds.
//
// Finds an NDI source whose name contains --name (the way CoreVideo Pro's NDI
// input discovers sources), receives it for --seconds, and prints one JSON
// line of what was MEASURED: resolution, frames, fps, audio sample rate,
// channels, seconds of audio, peak level. Exit 0 when frames arrived.
//
//   corevideo-edge-ndi-probe --name "CoreVideo Edge" --seconds 10

#include "edge/NdiRuntime.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace corevideo::edge;

int main(int argc, char** argv) {
  std::string name;
  int seconds = 10, findSeconds = 15;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--name") name = argv[i + 1];
    else if (key == "--seconds") seconds = std::atoi(argv[i + 1]);
    else if (key == "--find-seconds") findSeconds = std::atoi(argv[i + 1]);
  }
  std::string error;
  const auto* api = ndi::load(error);
  if (!api || !api->canReceive()) {
    std::printf("{\"ok\":false,\"error\":\"%s\"}\n", api ? "NDI receive API missing" : error.c_str());
    std::fflush(stdout);
    return 2;
  }
  ndi::FindCreate fc{true, nullptr, nullptr};
  void* finder = api->findCreate(&fc);
  std::string chosen, seen;
  ndi::Source source{};
  const auto findDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(findSeconds);
  while (chosen.empty() && std::chrono::steady_clock::now() < findDeadline) {
    api->findWait(finder, 1000);
    std::uint32_t count = 0;
    const ndi::Source* sources = api->findSources(finder, &count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (sources[i].name && seen.find(sources[i].name) == std::string::npos) seen += std::string(seen.empty() ? "" : " | ") + sources[i].name;
      if (sources[i].name && std::strstr(sources[i].name, name.c_str())) {
        chosen = sources[i].name;
        source = sources[i];
        break;
      }
    }
  }
  if (chosen.empty()) {
    api->findDestroy(finder);
    std::printf("{\"ok\":false,\"error\":\"no NDI source matching '%s' within %d s (seen: %s)\"}\n", name.c_str(),
                findSeconds, seen.c_str());
    std::fflush(stdout);
    return 1;
  }
  ndi::RecvCreate rc{source, 100, 100, false, "corevideo-edge-ndi-probe"};
  void* recv = api->recvCreate(&rc);
  api->findDestroy(finder);
  long long frames = 0, audioSamples = 0;
  int width = 0, height = 0, sampleRate = 0, channels = 0, fourcc = 0;
  float peak = 0.0f;
  bool sawMetadata = false;
  std::chrono::steady_clock::time_point firstFrame{}, lastFrame{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    ndi::Video v{};
    ndi::Audio a{};
    const int type = api->recvCapture(recv, &v, &a, nullptr, 100);
    if (type == ndi::kVideo) {
      const auto now = std::chrono::steady_clock::now();
      if (frames == 0) firstFrame = now;
      lastFrame = now;
      ++frames;
      width = v.width;
      height = v.height;
      fourcc = v.fourcc;
      if (v.metadata && std::strstr(v.metadata, "corevideo_edge")) sawMetadata = true;
      api->freeVideo(recv, &v);
    } else if (type == ndi::kAudio) {
      sampleRate = a.sampleRate;
      channels = a.channels;
      audioSamples += a.samples;
      for (int c = 0; c < a.channels; ++c) {
        const float* plane = reinterpret_cast<const float*>(reinterpret_cast<const char*>(a.data) + c * a.channelStrideBytes);
        for (int s = 0; s < a.samples; ++s) peak = std::max(peak, std::fabs(plane[s]));
      }
      api->freeAudio(recv, &a);
    }
  }
  api->recvDestroy(recv);
  const double span = frames > 1 ? std::chrono::duration<double>(lastFrame - firstFrame).count() : 0.0;
  const double fps = span > 0 ? (frames - 1) / span : 0.0;
  const double peakDb = peak > 0 ? 20.0 * std::log10(peak) : -100.0;
  std::printf("{\"ok\":%s,\"source\":\"%s\",\"width\":%d,\"height\":%d,\"fourcc\":%d,\"frames\":%lld,\"fps\":%.2f,"
              "\"audioRate\":%d,\"audioChannels\":%d,\"audioSeconds\":%.2f,\"audioPeakDb\":%.1f,\"metadata\":%s}\n",
              frames > 0 ? "true" : "false", chosen.c_str(), width, height, fourcc, frames, fps, sampleRate, channels,
              sampleRate ? static_cast<double>(audioSamples) / sampleRate : 0.0, peakDb, sawMetadata ? "true" : "false");
  std::fflush(stdout);
  return frames > 0 ? 0 : 1;
}
