#include "edge/SrtFeedSink.h"

#include "core/BoundedAsyncLog.h"
#include "edge/FastConform.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace corevideo::edge {

std::string findFfmpeg() {
#if defined(_WIN32)
  if (const char* dir = std::getenv("COREVIDEO_FFMPEG_DIR"); dir && *dir) {
    const auto p = std::filesystem::path(dir) / "ffmpeg.exe";
    if (std::filesystem::exists(p)) return p.string();
  }
  wchar_t found[MAX_PATH]{};
  if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0)
    return std::filesystem::path(found).string();
  return {};
#else
  if (const char* dir = std::getenv("COREVIDEO_FFMPEG_DIR"); dir && *dir) {
    const auto p = std::filesystem::path(dir) / "ffmpeg";
    if (std::filesystem::exists(p)) return p.string();
  }
  return {};
#endif
}

#if defined(_WIN32)

namespace {

constexpr int kW = 1920, kH = 1080, kFps = 30;
constexpr size_t kAudioQueueMax = 48000 * 2 * 2;  // 2 s of stereo floats

std::wstring widen(const std::string& s) { return std::filesystem::path(s).wstring(); }

class SrtSink final : public FeedSink {
 public:
  SrtSink(SrtSettings settings, int slot) : settings_(std::move(settings)), slot_(slot) {}
  ~SrtSink() override {
    running_ = false;
    audioCv_.notify_all();
    killProcess();
    if (supervisor_.joinable()) supervisor_.join();
  }

  bool open(std::string& error) override {
    ffmpeg_ = findFfmpeg();
    if (ffmpeg_.empty()) {
      error = "FFmpeg was not found (install it or set COREVIDEO_FFMPEG_DIR); SRT feeds need it.";
      return false;
    }
    supervisor_ = std::thread([this] { supervise(); });
    return true;
  }

  void video(const std::uint8_t* i420, int w, int h) override {
    std::lock_guard<std::mutex> lock(frameMutex_);
    conform_.run(i420, w, h, kW, kH, latest_);  // full-range NV12 1080p
    haveFrame_ = true;
  }

  void audio(const float* stereo, int frames) override {
    std::lock_guard<std::mutex> lock(audioMutex_);
    audioQueue_.insert(audioQueue_.end(), stereo, stereo + static_cast<size_t>(frames) * 2);
    if (audioQueue_.size() > kAudioQueueMax) {  // FFmpeg not reading: bounded, oldest goes
      audioQueue_.erase(audioQueue_.begin(), audioQueue_.begin() + static_cast<long long>(audioQueue_.size() - kAudioQueueMax));
    }
    audioCv_.notify_one();
  }

  std::string problem() override {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return problem_;
  }
  bool connected() override { return connected_.load(); }

 private:
  std::vector<std::wstring> argv(bool nvenc, const std::wstring& audioPipe) const {
    std::vector<std::wstring> a = {L"-hide_banner", L"-nostats", L"-loglevel", L"info",
                                   L"-fflags", L"nobuffer",
                                   L"-f", L"rawvideo", L"-pix_fmt", L"nv12", L"-s", L"1920x1080",
                                   L"-framerate", std::to_wstring(kFps),
                                   L"-i", L"pipe:0",
                                   L"-f", L"f32le", L"-ar", L"48000", L"-ac", L"2", L"-i", audioPipe,
                                   L"-map", L"0:v", L"-map", L"1:a",
                                   // Zoom delivers full range; broadcast receivers expect studio range.
                                   L"-vf", L"scale=in_range=full:out_range=tv,format=yuv420p"};
    if (nvenc) {
      for (const auto* s : {L"-c:v", L"h264_nvenc", L"-preset", L"p1", L"-tune", L"ll", L"-zerolatency", L"1",
                            L"-rc", L"cbr", L"-b:v", L"8M", L"-maxrate", L"8M", L"-bufsize", L"1M"})
        a.emplace_back(s);
    } else {
      for (const auto* s : {L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-tune", L"zerolatency",
                            L"-b:v", L"8M", L"-maxrate", L"8M", L"-bufsize", L"1M"})
        a.emplace_back(s);
    }
    for (const auto* s : {L"-g", L"30", L"-bf", L"0", L"-c:a", L"aac", L"-b:a", L"192k",
                          L"-ar", L"48000", L"-f", L"mpegts"})
      a.emplace_back(s);
    a.push_back(widen(srtUrl(settings_, slot_, false)));
    return a;
  }

  static std::wstring quote(const std::wstring& s) {
    if (s.find_first_of(L" \t\"&?") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (const wchar_t c : s) {
      if (c == L'"') out += L'\\';
      out += c;
    }
    return out + L"\"";
  }

  void setProblem(const std::string& p) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    problem_ = p;
  }

  void killProcess() {
    std::lock_guard<std::mutex> lock(processMutex_);
    if (process_) TerminateProcess(process_, 1);
  }

  void supervise() {
    bool nvenc = true;
    int attempt = 0;
    while (running_.load()) {
      ++attempt;
      const auto outcome = runOnce(nvenc, attempt);
      if (!running_.load()) break;
      if (outcome == Outcome::EncoderMissing && nvenc) { nvenc = false; continue; }
      // Pro not listening / connection lost: retry in 3 s, stated in health.
      for (int i = 0; i < 30 && running_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  enum class Outcome { Exited, EncoderMissing };

  Outcome runOnce(bool nvenc, int attempt) {
    connected_ = false;
    const std::wstring audioPipe = L"\\\\.\\pipe\\corevideo-edge-srt-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                   std::to_wstring(slot_) + L"-" + std::to_wstring(attempt);
    HANDLE audio = CreateNamedPipeW(audioPipe.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20,
                                    1 << 20, 0, nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE videoRead = nullptr, videoWrite = nullptr, logRead = nullptr, logWrite = nullptr;
    CreatePipe(&videoRead, &videoWrite, &sa, 8 << 20);
    CreatePipe(&logRead, &logWrite, &sa, 0);
    SetHandleInformation(videoWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(logRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote(widen(ffmpeg_));
    for (const auto& a : argv(nvenc, audioPipe)) cmd += L" " + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = videoRead;
    si.hStdOutput = logWrite;
    si.hStdError = logWrite;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(videoRead);
    CloseHandle(logWrite);
    if (!started) {
      setProblem("Could not start FFmpeg for the SRT feed.");
      CloseHandle(videoWrite);
      CloseHandle(logRead);
      CloseHandle(audio);
      return Outcome::Exited;
    }
    CloseHandle(pi.hThread);
    {
      std::lock_guard<std::mutex> lock(processMutex_);
      process_ = pi.hProcess;
    }
    std::atomic<bool> alive{true};
    std::atomic<bool> encoderMissing{false};
    std::string lastLine;

    std::thread log([&] {
      std::string line;
      char buf[1024];
      DWORD n = 0;
      while (ReadFile(logRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
          if (buf[i] == '\n' || buf[i] == '\r') {
            if (line.empty()) continue;
            if (line.find("Output #0") != std::string::npos) {
              connected_ = true;
              setProblem({});
            }
            if (line.find("h264_nvenc") != std::string::npos &&
                (line.find("Cannot load") != std::string::npos || line.find("No capable devices") != std::string::npos ||
                 line.find("Unknown encoder") != std::string::npos || line.find("not found") != std::string::npos))
              encoderMissing = true;
            const bool mentionsSrt = line.find("srt://") != std::string::npos;
            if (line.find("Connection setup failure") != std::string::npos ||
                line.find("connection timed out") != std::string::npos ||
                line.find("Connection refused") != std::string::npos ||
                (mentionsSrt && line.find("failed") != std::string::npos) ||
                line.find("Error opening output") != std::string::npos) {
              setProblem("Pro is not listening at " + settings_.host + ":" + std::to_string(srtPort(settings_, slot_)) +
                         " (turn on SRT input " + std::to_string(slot_) + " in CoreVideo Pro). Retrying.");
            }
            // FFmpeg echoes the full URL (passphrase included): never keep such a line.
            if (line.find("rror") != std::string::npos && !mentionsSrt) lastLine = line;
            line.clear();
          } else {
            line += buf[i];
          }
        }
      }
    });

    std::thread audioWriter([&] {
      if (!ConnectNamedPipe(audio, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
      std::vector<float> chunk;
      while (running_.load() && alive.load()) {
        {
          std::unique_lock<std::mutex> lock(audioMutex_);
          audioCv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return !audioQueue_.empty() || !running_.load(); });
          chunk.assign(audioQueue_.begin(), audioQueue_.end());
          audioQueue_.clear();
        }
        if (chunk.empty()) continue;
        DWORD written = 0;
        if (!WriteFile(audio, chunk.data(), static_cast<DWORD>(chunk.size() * sizeof(float)), &written, nullptr)) break;
      }
    });

    // Video pacer: a constant 30 fps of the latest picture (repeat when Zoom
    // is slower, newest wins when faster). Starts once the first frame exists.
    std::vector<std::uint8_t> frame;
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
      {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (haveFrame_) frame = latest_;
      }
      if (!frame.empty()) {
        DWORD written = 0;
        if (!WriteFile(videoWrite, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr)) break;
      }
      next += std::chrono::microseconds(1'000'000 / kFps);
      std::this_thread::sleep_until(next);
      if (std::chrono::steady_clock::now() - next > std::chrono::seconds(1)) next = std::chrono::steady_clock::now();
    }
    alive = false;
    CloseHandle(videoWrite);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    {
      std::lock_guard<std::mutex> lock(processMutex_);
      process_ = nullptr;
    }
    CloseHandle(pi.hProcess);
    // Unblock a ConnectNamedPipe that never got a client.
    HANDLE poke = CreateFileW(audioPipe.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
    audioCv_.notify_all();
    audioWriter.join();
    CloseHandle(audio);
    log.join();
    CloseHandle(logRead);
    connected_ = false;
    if (running_.load()) {
      std::lock_guard<std::mutex> lock(stateMutex_);
      if (problem_.empty()) problem_ = lastLine.empty() ? std::string("The SRT feed stopped. Retrying.") : "SRT: " + lastLine;
      core::nativeLogf("[feeds] srt slot %d: %s\n", slot_, problem_.c_str());
    }
    return encoderMissing.load() ? Outcome::EncoderMissing : Outcome::Exited;
  }

  SrtSettings settings_;
  int slot_;
  std::string ffmpeg_;
  std::atomic<bool> running_{true};
  std::atomic<bool> connected_{false};
  std::thread supervisor_;
  std::mutex processMutex_;
  HANDLE process_ = nullptr;
  std::mutex stateMutex_;
  std::string problem_;
  std::mutex frameMutex_;
  FastConform conform_;
  std::vector<std::uint8_t> latest_;
  bool haveFrame_ = false;
  std::mutex audioMutex_;
  std::condition_variable audioCv_;
  std::vector<float> audioQueue_;
};

}  // namespace

std::unique_ptr<FeedSink> makeSrtFeedSink(const SrtSettings& settings, int slot) {
  return std::make_unique<SrtSink>(settings, slot);
}

#else

namespace {
class UnsupportedSrtSink final : public FeedSink {
 public:
  bool open(std::string& error) override {
    error = "SRT feeds are not built for this platform yet (Windows only in this build).";
    return false;
  }
  void video(const std::uint8_t*, int, int) override {}
  void audio(const float*, int) override {}
};
}  // namespace

std::unique_ptr<FeedSink> makeSrtFeedSink(const SrtSettings&, int) { return std::make_unique<UnsupportedSrtSink>(); }

#endif

}  // namespace corevideo::edge
