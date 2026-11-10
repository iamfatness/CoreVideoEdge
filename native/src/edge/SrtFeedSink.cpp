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
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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
  const auto runnable = [](const std::filesystem::path& p) { return access(p.c_str(), X_OK) == 0; };
  if (const char* dir = std::getenv("COREVIDEO_FFMPEG_DIR"); dir && *dir) {
    const auto p = std::filesystem::path(dir) / "ffmpeg";
    if (runnable(p)) return p.string();
  }
  if (const char* path = std::getenv("PATH")) {
    const std::string list(path);
    size_t start = 0;
    while (start <= list.size()) {
      const auto end = list.find(':', start);
      const auto dir = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!dir.empty() && runnable(std::filesystem::path(dir) / "ffmpeg")) return (std::filesystem::path(dir) / "ffmpeg").string();
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  // An app started from Finder gets a minimal PATH without Homebrew.
  for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin"}) {
    if (runnable(std::filesystem::path(dir) / "ffmpeg")) return (std::filesystem::path(dir) / "ffmpeg").string();
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
    if (audio == INVALID_HANDLE_VALUE) {
      setProblem("Could not create the SRT feed's audio pipe.");
      return Outcome::Exited;
    }
    // Up to 8 slots launch FFmpeg at once. Every pipe is created
    // NON-inheritable and only this child's two ends are passed, through an
    // explicit handle list: otherwise one slot's FFmpeg inherits another
    // slot's pipe ends, the other slot never sees EOF / broken pipe, and a
    // reconfigure deadlocks the node (review 2026-11-09).
    HANDLE videoRead = nullptr, videoWrite = nullptr, logRead = nullptr, logWrite = nullptr;
    PROCESS_INFORMATION pi{};
    BOOL started = FALSE;
    {
      static std::mutex launchMutex;
      std::lock_guard<std::mutex> launch(launchMutex);
      if (CreatePipe(&videoRead, &videoWrite, nullptr, 8 << 20) && CreatePipe(&logRead, &logWrite, nullptr, 0) &&
          SetHandleInformation(videoRead, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) &&
          SetHandleInformation(logWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        std::wstring cmd = quote(widen(ffmpeg_));
        for (const auto& a : argv(nvenc, audioPipe)) cmd += L" " + quote(a);
        HANDLE inherit[2] = {videoRead, logWrite};
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<unsigned char> attrBuffer(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
        if (InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
                                      nullptr)) {
          STARTUPINFOEXW si{};
          si.StartupInfo.cb = sizeof(si);
          si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
          si.StartupInfo.hStdInput = videoRead;
          si.StartupInfo.hStdOutput = logWrite;
          si.StartupInfo.hStdError = logWrite;
          si.lpAttributeList = attrs;
          started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
          DeleteProcThreadAttributeList(attrs);
        }
      }
      if (videoRead) CloseHandle(videoRead);
      if (logWrite) CloseHandle(logWrite);
    }
    if (!started) {
      setProblem("Could not start FFmpeg for the SRT feed.");
      if (videoWrite) CloseHandle(videoWrite);
      if (logRead) CloseHandle(logRead);
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
            // Any NVENC failure falls back to x264: missing driver, no device,
            // and the common consumer-GPU one, out of encode sessions
            // ("OpenEncodeSessionEx failed: out of memory") once the ISOs
            // already hold several (review 2026-11-09).
            if ((line.find("nvenc") != std::string::npos || line.find("OpenEncodeSessionEx") != std::string::npos) &&
                (line.find("Cannot load") != std::string::npos || line.find("No capable devices") != std::string::npos ||
                 line.find("Unknown encoder") != std::string::npos || line.find("not found") != std::string::npos ||
                 line.find("failed") != std::string::npos || line.find("rror") != std::string::npos))
              encoderMissing = true;
            if (line.find("Error while opening encoder") != std::string::npos ||
                line.find("Error initializing output stream") != std::string::npos)
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
    // is slower, newest wins when faster). A guest with their camera off has
    // no frames at all; after 1 s the feed carries black so FFmpeg starts and
    // the guest's audio still reaches Pro (review 2026-11-09).
    std::vector<std::uint8_t> frame;
    const auto launched = std::chrono::steady_clock::now();
    auto next = launched;
    while (running_.load()) {
      if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
      {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (haveFrame_) frame = latest_;
      }
      if (frame.empty() && std::chrono::steady_clock::now() - launched > std::chrono::seconds(1)) {
        frame.assign(static_cast<size_t>(kW) * kH * 3 / 2, 128);
        std::fill(frame.begin(), frame.begin() + static_cast<long long>(kW) * kH, std::uint8_t{0});  // full-range black
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

#else  // POSIX (macOS)

namespace {

constexpr int kW = 1920, kH = 1080, kFps = 30;
constexpr size_t kAudioQueueMax = 48000 * 2 * 2;  // 2 s of stereo floats

bool writeAll(int fd, const void* data, size_t bytes) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (bytes > 0) {
    const auto n = ::write(fd, p, bytes);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;  // EPIPE: FFmpeg went away (SIGPIPE is ignored in the node)
    p += n;
    bytes -= static_cast<size_t>(n);
  }
  return true;
}

void closeFd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

// Same feed as the Windows sink; the plumbing is POSIX: FFmpeg reads video on
// stdin and audio on fd 3 ("pipe:3", Pro's route), and writes its log to one
// pipe. Handle law (CLAUDE.md, cross-slot leaks): every pipe is created
// close-on-exec under one launch mutex, the child gets exactly its three ends
// through posix_spawn file actions, and POSIX_SPAWN_CLOEXEC_DEFAULT closes
// everything else in the child, so one slot's FFmpeg can never hold another
// slot's pipe open.
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
      error = "FFmpeg was not found (install it, e.g. brew install ffmpeg, or set COREVIDEO_FFMPEG_DIR); SRT feeds need it.";
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
  std::vector<std::string> argv(bool hardware) const {
    std::vector<std::string> a = {ffmpeg_, "-hide_banner", "-nostats", "-loglevel", "info",
                                  "-fflags", "nobuffer",
                                  "-f", "rawvideo", "-pix_fmt", "nv12", "-s", "1920x1080",
                                  "-framerate", std::to_string(kFps),
                                  "-i", "pipe:0",
                                  "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", "pipe:3",
                                  "-map", "0:v", "-map", "1:a",
                                  // Zoom delivers full range; broadcast receivers expect studio range.
                                  "-vf", "scale=in_range=full:out_range=tv,format=yuv420p"};
    if (hardware) {
      for (const char* s : {"-c:v", "h264_videotoolbox", "-realtime", "1", "-profile:v", "high",
                            "-b:v", "8M", "-maxrate", "8M", "-bufsize", "1M"})
        a.emplace_back(s);
    } else {
      for (const char* s : {"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                            "-b:v", "8M", "-maxrate", "8M", "-bufsize", "1M"})
        a.emplace_back(s);
    }
    for (const char* s : {"-g", "30", "-bf", "0", "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-f", "mpegts"})
      a.emplace_back(s);
    a.push_back(srtUrl(settings_, slot_, false));
    return a;
  }

  void setProblem(const std::string& p) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    problem_ = p;
  }

  void killProcess() {
    std::lock_guard<std::mutex> lock(processMutex_);
    if (pid_ > 0) ::kill(pid_, SIGKILL);
  }

  void supervise() {
    bool hardware = true;
    while (running_.load()) {
      const auto outcome = runOnce(hardware);
      if (!running_.load()) break;
      if (outcome == Outcome::EncoderMissing && hardware) { hardware = false; continue; }
      // Pro not listening / connection lost: retry in 3 s, stated in health.
      for (int i = 0; i < 30 && running_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  enum class Outcome { Exited, EncoderMissing };

  // Spawns FFmpeg with exactly its three pipe ends. Returns the pid or -1.
  pid_t spawn(bool hardware, int& videoWrite, int& audioWrite, int& logRead) {
    static std::mutex launchMutex;
    std::lock_guard<std::mutex> launch(launchMutex);
    int video[2] = {-1, -1}, audio[2] = {-1, -1}, log[2] = {-1, -1};
    const auto cloexec = [](int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0; };
    bool ok = pipe(video) == 0 && pipe(audio) == 0 && pipe(log) == 0;
    for (int fd : {video[0], video[1], audio[0], audio[1], log[0], log[1]}) ok = ok && fd >= 0 && cloexec(fd);
    pid_t pid = -1;
    if (ok) {
      posix_spawn_file_actions_t actions;
      posix_spawnattr_t attr;
      posix_spawn_file_actions_init(&actions);
      posix_spawnattr_init(&attr);
      posix_spawn_file_actions_adddup2(&actions, video[0], 0);
      posix_spawn_file_actions_adddup2(&actions, log[1], 1);
      posix_spawn_file_actions_adddup2(&actions, log[1], 2);
      posix_spawn_file_actions_adddup2(&actions, audio[0], 3);
      sigset_t defaults, none;
      sigemptyset(&defaults);
      sigaddset(&defaults, SIGPIPE);  // the node ignores SIGPIPE; FFmpeg gets the default back
      sigemptyset(&none);
      posix_spawnattr_setsigdefault(&attr, &defaults);
      posix_spawnattr_setsigmask(&attr, &none);
      short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
      flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // Apple: nothing but the dup2'd ends crosses exec
#endif
      posix_spawnattr_setflags(&attr, flags);
      const auto args = argv(hardware);
      std::vector<char*> raw;
      for (const auto& s : args) raw.push_back(const_cast<char*>(s.c_str()));
      raw.push_back(nullptr);
      if (posix_spawn(&pid, ffmpeg_.c_str(), &actions, &attr, raw.data(), environ) != 0) pid = -1;
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attr);
    }
    // The child's ends now live only in the child.
    closeFd(video[0]);
    closeFd(audio[0]);
    closeFd(log[1]);
    if (pid <= 0) {
      closeFd(video[1]);
      closeFd(audio[1]);
      closeFd(log[0]);
      return -1;
    }
    videoWrite = video[1];
    audioWrite = audio[1];
    logRead = log[0];
    return pid;
  }

  Outcome runOnce(bool hardware) {
    connected_ = false;
    int videoWrite = -1, audioWrite = -1, logRead = -1;
    const pid_t pid = spawn(hardware, videoWrite, audioWrite, logRead);
    if (pid <= 0) {
      setProblem("Could not start FFmpeg for the SRT feed.");
      return Outcome::Exited;
    }
    {
      std::lock_guard<std::mutex> lock(processMutex_);
      pid_ = pid;
    }
    std::atomic<bool> alive{true};
    std::atomic<bool> encoderMissing{false};
    std::string lastLine;

    std::thread log([&] {
      std::string line;
      char buf[1024];
      for (;;) {
        const auto n = ::read(logRead, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
          if (buf[i] == '\n' || buf[i] == '\r') {
            if (line.empty()) continue;
            if (line.find("Output #0") != std::string::npos) {
              connected_ = true;
              setProblem({});
            }
            // A VideoToolbox encoder that cannot open (no hardware session in
            // a VM, out of sessions) falls back to x264, as NVENC does on Windows.
            if (line.find("videotoolbox") != std::string::npos &&
                (line.find("rror") != std::string::npos || line.find("failed") != std::string::npos ||
                 line.find("cannot") != std::string::npos || line.find("Unknown encoder") != std::string::npos))
              encoderMissing = true;
            if (line.find("Error while opening encoder") != std::string::npos ||
                line.find("Error initializing output stream") != std::string::npos ||
                line.find("Unknown encoder") != std::string::npos)
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
      std::vector<float> chunk;
      while (running_.load() && alive.load()) {
        {
          std::unique_lock<std::mutex> lock(audioMutex_);
          audioCv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return !audioQueue_.empty() || !running_.load(); });
          chunk.assign(audioQueue_.begin(), audioQueue_.end());
          audioQueue_.clear();
        }
        if (chunk.empty()) continue;
        if (!writeAll(audioWrite, chunk.data(), chunk.size() * sizeof(float))) break;
      }
    });

    // Video pacer: a constant 30 fps of the latest picture (repeat when Zoom
    // is slower, newest wins when faster); black after 1 s with no camera so
    // the guest's audio still reaches Pro (same rules as Windows).
    std::vector<std::uint8_t> frame;
    const auto launched = std::chrono::steady_clock::now();
    auto next = launched;
    bool reaped = false;
    int status = 0;
    while (running_.load()) {
      if (waitpid(pid, &status, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (haveFrame_) frame = latest_;
      }
      if (frame.empty() && std::chrono::steady_clock::now() - launched > std::chrono::seconds(1)) {
        frame.assign(static_cast<size_t>(kW) * kH * 3 / 2, 128);
        std::fill(frame.begin(), frame.begin() + static_cast<long long>(kW) * kH, std::uint8_t{0});  // full-range black
      }
      if (!frame.empty() && !writeAll(videoWrite, frame.data(), frame.size())) break;
      next += std::chrono::microseconds(1'000'000 / kFps);
      std::this_thread::sleep_until(next);
      if (std::chrono::steady_clock::now() - next > std::chrono::seconds(1)) next = std::chrono::steady_clock::now();
    }
    alive = false;
    closeFd(videoWrite);
    if (!reaped) {
      ::kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    {
      std::lock_guard<std::mutex> lock(processMutex_);
      pid_ = -1;
    }
    audioCv_.notify_all();
    audioWriter.join();  // its pipe's reader is gone: any blocked write returns EPIPE
    closeFd(audioWrite);
    log.join();
    closeFd(logRead);
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
  pid_t pid_ = -1;
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

#endif

}  // namespace corevideo::edge
