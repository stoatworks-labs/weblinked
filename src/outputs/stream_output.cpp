// RTMP/SRT sender: the page, encoded, pushed to a streaming server.
//
// This is the output that lets WebLinked feed a Restreamer (datarhei Core), a
// YouTube ingest, or anything else that speaks RTMP or SRT — the one shape of
// destination the other backends cannot reach, because SDI needs a card and
// NDI/OMT need a receiver on the same network.
//
// ## Why a subprocess rather than libavformat
//
// Linking FFmpeg would put an LGPL/GPL dependency, a codec-licensing question
// and about 40 MB into a binary whose entire job is rendering a web page. An
// `ffmpeg` process is a stable, documented interface, it is already on every
// machine that does streaming work, and when it fails it says why in a line of
// text an operator can read. The cost is that this backend is unavailable when
// ffmpeg is not installed, which start() reports plainly.
//
// ## Why two TCP sockets rather than pipes
//
// ffmpeg needs video and audio as two separate inputs, and there is no portable
// way to hand a child process two extra pipes: `pipe:3` works on POSIX and does
// not on Windows. `tcp://127.0.0.1:port` behaves identically everywhere, and
// ffmpeg is the client so we never have to guess when it is ready. Both
// listeners bind to loopback only and stop listening the moment ffmpeg
// connects, so the window in which another local process could connect first is
// one connection wide.
//
// ## Sync
//
// Neither raw stream carries timestamps: ffmpeg derives video pts from the
// frame count at `-r` and audio pts from the sample count at `-ar`. So the two
// stay in sync exactly as long as we write *every* tick to both. That is why
// the queue holds video and audio together as one pair and drops them together
// — dropping a video frame alone would move the picture permanently ahead of
// the sound, which is far worse than a dropped frame and much harder to spot.
//
// NOT verified against a real streaming server: this compiles and its argument
// construction is unit-tested, but no RTMP endpoint has consumed its output.
// See docs/04-verification.md.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define WL_INVALID_SOCKET INVALID_SOCKET
#define WL_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using socket_t = int;
#define WL_INVALID_SOCKET (-1)
#define WL_CLOSE_SOCKET ::close
extern char** environ;
#endif

#include "diag/diag.h"
#include "core/socket_inherit.h"
#include "core/stream_args.h"
#include "outputs/output.h"

namespace weblinked {
namespace {

#if defined(_WIN32)
struct WinsockGuard {
  WinsockGuard() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockGuard() { WSACleanup(); }
};
void ensureWinsock() { static WinsockGuard guard; }
#else
void ensureWinsock() {}
#endif

/// Read a stream output's settings out of its spec's `options`. Every field has
/// a working default, so `{"kind":"stream","options":{"url":"rtmp://…"}}` is a
/// complete configuration.
StreamSettings streamSettingsFrom(const OutputSpec& spec) {
  StreamSettings settings;
  settings.url = spec.optionString("url");
  settings.container = spec.optionString("container");
  settings.ffmpegPath = spec.optionString("ffmpeg", "ffmpeg");
  settings.videoCodec = spec.optionString("codec", "libx264");
  settings.videoBitrate = spec.optionString("bitrate", "6000k");
  settings.preset = spec.optionString("preset", "veryfast");
  settings.tune = spec.optionString("tune");
  settings.audioCodec = spec.optionString("audio_codec", "aac");
  settings.audioBitrate = spec.optionString("audio_bitrate", "128k");
  settings.audioChannels = spec.optionInt("audio_channels", 2);
  settings.audioSampleRate = spec.optionInt("audio_sample_rate", 48000);
  settings.gopSeconds = spec.optionInt("gop_seconds", 2);
  return settings;
}

/// A loopback listener that accepts exactly one connection and then writes to
/// it. ffmpeg is the client for both video and audio.
class OneShotListener {
 public:
  ~OneShotListener() { close(); }

  /// Bind to 127.0.0.1 on a kernel-chosen port. Returns the port, or 0.
  int listenOnEphemeralPort() {
    ensureWinsock();
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ == WL_INVALID_SOCKET) {
      return 0;
    }
    // ffmpeg reaches these by port number, never by inheriting the descriptor,
    // so nothing is lost by keeping them out of the child.
    preventSocketInheritance(static_cast<std::intptr_t>(listener_));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener_, 1) != 0) {
      closeListener();
      return 0;
    }
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      closeListener();
      return 0;
    }
    return static_cast<int>(ntohs(addr.sin_port));
  }

  /// Block until ffmpeg connects. Called on the writer thread only, never on
  /// the clock thread — an output that blocked start() would hold up every
  /// other output on the same engine.
  bool accept() {
    if (listener_ == WL_INVALID_SOCKET) {
      return false;
    }
    peer_ = ::accept(listener_, nullptr, nullptr);
    closeListener();
    if (peer_ == WL_INVALID_SOCKET) {
      return false;
    }
    preventSocketInheritance(static_cast<std::intptr_t>(peer_));
    int one = 1;
    ::setsockopt(peer_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    // A send timeout so a wedged ffmpeg cannot hold shutdown open forever. Two
    // seconds is far longer than any healthy write and short enough that
    // stopping an output still feels immediate.
#if defined(_WIN32)
    DWORD timeout = 2000;
    ::setsockopt(peer_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
#else
    timeval timeout{2, 0};
    ::setsockopt(peer_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    return true;
  }

  /// Write everything or fail. A short write is a full socket buffer, not an
  /// error, so it loops.
  bool writeAll(const uint8_t* data, size_t bytes) {
    size_t written = 0;
    while (written < bytes) {
#if defined(_WIN32)
      const int n = ::send(peer_, reinterpret_cast<const char*>(data + written),
                           static_cast<int>(bytes - written), 0);
#else
      const ssize_t n = ::send(peer_, data + written, bytes - written, MSG_NOSIGNAL);
#endif
      if (n <= 0) {
        return false;
      }
      written += static_cast<size_t>(n);
    }
    return true;
  }

  /// Unblock a writer parked in accept() without touching an established
  /// connection, so shutdown can let the queues drain before it cuts the wire.
  void stopAccepting() { closeListener(); }

  void close() {
    closeListener();
    if (peer_ != WL_INVALID_SOCKET) {
      WL_CLOSE_SOCKET(peer_);
      peer_ = WL_INVALID_SOCKET;
    }
  }

 private:
  void closeListener() {
    if (listener_ != WL_INVALID_SOCKET) {
      WL_CLOSE_SOCKET(listener_);
      listener_ = WL_INVALID_SOCKET;
    }
  }

  socket_t listener_ = WL_INVALID_SOCKET;
  socket_t peer_ = WL_INVALID_SOCKET;
};

/// The ffmpeg child, plus a reader that keeps its most recent stderr line —
/// which is where "Connection refused" and "Server returned 403" come from, and
/// the only thing that makes a failed stream diagnosable from the control page.
class FfmpegProcess {
 public:
  ~FfmpegProcess() { terminate(); }

  bool spawn(const std::vector<std::string>& argv, std::string& error) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
      error = "could not create a pipe for ffmpeg's output";
      return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    std::string commandLine;
    for (const auto& arg : argv) {
      if (!commandLine.empty()) {
        commandLine += ' ';
      }
      // Quote anything with a space; ffmpeg arguments never contain quotes of
      // their own, and a URL with a space in it is already malformed.
      commandLine += (arg.find(' ') == std::string::npos) ? arg : ("\"" + arg + "\"");
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = writeEnd;
    si.hStdOutput = writeEnd;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
      CloseHandle(readEnd);
      error = "could not start ffmpeg (is it on PATH?)";
      return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = static_cast<int>(pi.dwProcessId);
    logPipe_ = readEnd;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      error = "could not create a pipe for ffmpeg's output";
      return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);

    pid_t pid = 0;
    // posix_spawnp rather than fork/exec: this process is full of CEF threads,
    // and fork() in a threaded process is only safe for async-signal-safe calls.
    const int rc = ::posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
      ::close(fds[0]);
      error = "could not start ffmpeg (is it on PATH?)";
      return false;
    }
    pid_ = static_cast<int>(pid);
    logPipe_ = fds[0];
#endif

    logReader_ = std::thread([this] { readLog(); });
    return true;
  }

  bool running() const {
#if defined(_WIN32)
    if (process_ == nullptr) {
      return false;
    }
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    if (pid_ <= 0) {
      return false;
    }
    int status = 0;
    return ::waitpid(pid_, &status, WNOHANG) == 0;
#endif
  }

  /// Wait for ffmpeg to finish on its own. Closing both input sockets is an EOF,
  /// which makes it flush the muxer and exit — so the *correct* shutdown is to
  /// wait here, not to signal. Signalling first truncates whatever the encoder
  /// still holds, which measured as about 300 ms of missing audio against a
  /// full-length video.
  bool waitForExit(int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 25) {
      if (!running()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return !running();
  }

  void terminate() {
#if defined(_WIN32)
    if (process_ != nullptr) {
      TerminateProcess(process_, 0);
      WaitForSingleObject(process_, 2000);
      CloseHandle(process_);
      process_ = nullptr;
    }
    if (logPipe_ != nullptr) {
      CloseHandle(logPipe_);
      logPipe_ = nullptr;
    }
#else
    if (pid_ > 0) {
      // SIGINT, not SIGKILL: ffmpeg flushes the muxer and closes the RTMP
      // connection cleanly, so the server sees a stream that ended rather than
      // one that vanished and may be reconnected to.
      ::kill(pid_, SIGINT);
      for (int i = 0; i < 40 && running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (running()) {
        ::kill(pid_, SIGKILL);
      }
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
    if (logPipe_ >= 0) {
      ::close(logPipe_);
      logPipe_ = -1;
    }
#endif
    if (logReader_.joinable()) {
      logReader_.join();
    }
  }

  int pid() const { return pid_; }

  std::string lastLine() const {
    std::lock_guard<std::mutex> lock(logMutex_);
    return lastLine_;
  }

 private:
  void readLog() {
    char buffer[512];
    std::string pending;
    for (;;) {
#if defined(_WIN32)
      DWORD read = 0;
      if (logPipe_ == nullptr || !ReadFile(logPipe_, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
        break;
      }
      const size_t count = static_cast<size_t>(read);
#else
      const ssize_t read = ::read(logPipe_, buffer, sizeof(buffer));
      if (read <= 0) {
        break;
      }
      const size_t count = static_cast<size_t>(read);
#endif
      pending.append(buffer, count);
      // ffmpeg's progress lines end in '\r', its messages in '\n'.
      size_t cut = pending.find_last_of("\r\n");
      while (cut != std::string::npos) {
        const size_t start = pending.find_last_of("\r\n", cut > 0 ? cut - 1 : 0);
        std::string line = (start == std::string::npos)
                               ? pending.substr(0, cut)
                               : pending.substr(start + 1, cut - start - 1);
        if (!line.empty()) {
          std::lock_guard<std::mutex> lock(logMutex_);
          lastLine_ = line;
        }
        pending.erase(0, cut + 1);
        cut = pending.find_last_of("\r\n");
      }
      if (pending.size() > 4096) {
        pending.clear();
      }
    }
  }

  int pid_ = -1;
#if defined(_WIN32)
  HANDLE process_ = nullptr;
  HANDLE logPipe_ = nullptr;
#else
  int logPipe_ = -1;
#endif
  std::thread logReader_;
  mutable std::mutex logMutex_;
  std::string lastLine_;
};

/// One tick: the picture and the audio that belongs with it, kept together so
/// they can only ever be dropped together. See the sync note at the top.
struct Tick {
  std::vector<uint8_t> video;
  std::vector<uint8_t> audio;
};

class StreamOutput : public IOutput {
 public:
  explicit StreamOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "stream" : spec.name), spec_(spec) {}

  ~StreamOutput() override { stop(); }

  std::string kind() const override { return "stream"; }

  // UYVY costs half of BGRA on the wire and is the format the SDI and NDI
  // backends already ask for, so on an engine with either of those the
  // conversion is shared rather than paid for twice.
  PixelFormat pixelFormat() const override { return PixelFormat::kUYVY; }

  bool wantsAudio() const override { return true; }

  // FLV and MPEG-TS carry no alpha, so there is nothing to un-premultiply.
  bool wantsStraightAlpha() const override { return false; }

  bool start(const VideoFormat& format, std::string& error) override {
    const StreamSettings settings = streamSettingsFrom(spec_);
    if (settings.url.empty()) {
      error = "a stream output needs a url option, e.g. rtmp://host:1935/live/key";
      return false;
    }
    settings_ = settings;
    format_ = format;
    frameBytes_ = static_cast<size_t>(format.width) * static_cast<size_t>(format.height) * 2;

    const int videoPort = videoPipe_.listenOnEphemeralPort();
    const int audioPort = audioPipe_.listenOnEphemeralPort();
    if (videoPort == 0 || audioPort == 0) {
      error = "could not open a loopback socket for ffmpeg";
      videoPipe_.close();
      audioPipe_.close();
      return false;
    }

    const std::vector<std::string> argv =
        buildFfmpegArgs(settings_, format, videoPort, audioPort);
    if (!ffmpeg_.spawn(argv, error)) {
      videoPipe_.close();
      audioPipe_.close();
      return false;
    }

    stopping_ = false;
    videoDrained_ = false;
    videoConnected_ = false;
    audioConnected_ = false;
    // Two writers, not one. ffmpeg opens its inputs in order and probes each as
    // it goes, so it will not even connect to the audio socket until video data
    // is flowing — waiting for both connections before writing either is a
    // deadlock, and was exactly how the first version of this failed.
    videoWriter_ = std::thread([this] { videoLoop(); });
    audioWriter_ = std::thread([this] { audioLoop(); });
    running_ = true;
    diag::info("stream '%s': publishing to %s (ffmpeg pid %d)", name_.c_str(),
               redactStreamUrl(settings_.url).c_str(), ffmpeg_.pid());
    return true;
  }

  void stop() override {
    if (!running_ && !videoWriter_.joinable() && !audioWriter_.joinable()) {
      return;
    }
    running_ = false;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      stopping_ = true;
    }
    queueSignal_.notify_all();
    // Stop accepting but leave established connections open, so both writers
    // drain what is already queued before the wire is cut. Closing the sockets
    // here instead truncates the tail — it cost half a second of audio against
    // a full-length video, which is exactly the silent desync this backend is
    // otherwise careful to avoid. A writer parked in accept() still leaves,
    // because the listener is what it is waiting on.
    videoPipe_.stopAccepting();
    audioPipe_.stopAccepting();
    if (videoWriter_.joinable()) {
      videoWriter_.join();
    }
    if (audioWriter_.joinable()) {
      audioWriter_.join();
    }
    // Closing both sockets is the EOF that tells ffmpeg to wrap up. Give it a
    // moment to do so before terminate() escalates to a signal.
    videoPipe_.close();
    audioPipe_.close();
    ffmpeg_.waitForExit(3000);
    ffmpeg_.terminate();
    videoConnected_ = false;
    audioConnected_ = false;
  }

  void submit(const VideoFrame& video, const AudioBlock& audio) override {
    if (!running_ || video.sizeBytes() != frameBytes_) {
      return;
    }

    Tick tick;
    tick.video.assign(video.data(), video.data() + video.sizeBytes());
    if (audio.valid() && audio.interleaved != nullptr &&
        audio.channels == settings_.audioChannels) {
      const size_t samples = static_cast<size_t>(audio.frames) * static_cast<size_t>(audio.channels);
      const auto* bytes = reinterpret_cast<const uint8_t*>(audio.interleaved);
      tick.audio.assign(bytes, bytes + samples * sizeof(float));
    } else {
      // Silence of the right length, so the audio stream keeps advancing at the
      // same rate as the video even on a page that produces nothing.
      const int frames = audio.valid() ? audio.frames : expectedAudioFrames();
      tick.audio.assign(static_cast<size_t>(frames) * static_cast<size_t>(settings_.audioChannels) *
                            sizeof(float),
                        0);
      if (audio.valid() && audio.channels != settings_.audioChannels && !channelMismatchLogged_) {
        channelMismatchLogged_ = true;
        diag::error("stream '%s': the page has %d audio channels but the output was "
                    "opened for %d — sending silence instead of going out of sync",
                    name_.c_str(), audio.channels, settings_.audioChannels);
      }
    }

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (queue_.size() >= kMaxQueuedTicks) {
        // Oldest first: the newest picture is the one worth keeping, and
        // dropping the pair keeps sound and picture aligned.
        queue_.pop_front();
        dropped_++;
      }
      queue_.push_back(std::move(tick));
    }
    queueSignal_.notify_all();
  }

  json::Value status() const override {
    json::Value out = json::Value::object();
    out.set("kind", json::Value(kind()));
    out.set("name", json::Value(name_));
    out.set("running", json::Value(running_ && ffmpegAlive()));
    out.set("url", json::Value(redactStreamUrl(settings_.url)));
    out.set("connected", json::Value(videoConnected_.load() && audioConnected_.load()));
    out.set("frames", json::Value(static_cast<double>(frames_.load())));
    out.set("dropped", json::Value(static_cast<double>(dropped_.load())));
    // Audio and video are only in sync while these two agree: one tick of video
    // must carry exactly one tick of audio. `audio_deficit_ms` is that
    // comparison made explicit, because a slow drift is invisible in any single
    // number and obvious in this one.
    const int64_t sent = audioSamples_.load();
    out.set("audio_samples", json::Value(static_cast<double>(sent)));
    if (format_.rate.numerator > 0 && settings_.audioSampleRate > 0) {
      const double videoSeconds = static_cast<double>(frames_.load()) *
                                  format_.rate.denominator / format_.rate.numerator;
      const double audioSeconds = static_cast<double>(sent) / settings_.audioSampleRate;
      out.set("audio_deficit_ms", json::Value((videoSeconds - audioSeconds) * 1000.0));
    }
    out.set("queued", json::Value(static_cast<double>(queueDepth())));
    out.set("ffmpeg_pid", json::Value(static_cast<double>(ffmpeg_.pid())));
    const std::string last = ffmpeg_.lastLine();
    if (!last.empty()) {
      out.set("last_logline", json::Value(last));
    }
    if (!error_.empty()) {
      out.set("error", json::Value(error_));
    }
    return out;
  }

 private:
  static constexpr size_t kMaxQueuedTicks = 30;
  /// Generous: this only fills during ffmpeg's startup probe of the video
  /// input, which is a fraction of a second, and draining it late costs
  /// nothing because the audio still starts from its own first sample.
  static constexpr size_t kMaxPendingAudio = 250;

  bool ffmpegAlive() const { return const_cast<FfmpegProcess&>(ffmpeg_).running(); }

  size_t queueDepth() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return queue_.size();
  }

  /// Nominal samples for one frame. Only used for a page with no audio at all,
  /// where being a sample out per frame is harmless — a page that *does*
  /// produce audio supplies the exact 800/801 alternation itself.
  int expectedAudioFrames() const {
    if (format_.rate.numerator <= 0) {
      return 0;
    }
    return static_cast<int>(static_cast<int64_t>(settings_.audioSampleRate) *
                            format_.rate.denominator / format_.rate.numerator);
  }

  /// Video: accept, then write every frame. This is the stream that has to move
  /// first — see the comment in start().
  ///
  /// Whichever way it leaves, it marks itself drained and wakes the audio
  /// writer: that thread's queue is fed from here, so without this it would
  /// wait for a hand-over that is never coming.
  void videoLoop() {
    videoLoopBody();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      videoDrained_ = true;
    }
    queueSignal_.notify_all();
  }

  void videoLoopBody() {
    if (!videoPipe_.accept()) {
      fail("ffmpeg never connected to the video socket");
      return;
    }
    videoConnected_ = true;

    for (;;) {
      Tick tick;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueSignal_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;  // stopping, and everything queued has been written
        }
        tick = std::move(queue_.front());
        queue_.pop_front();
      }

      if (!videoPipe_.writeAll(tick.video.data(), tick.video.size())) {
        fail("ffmpeg closed the video connection");
        return;
      }
      frames_++;

      // Hand the matching audio to the other writer rather than dropping it.
      // ffmpeg timestamps audio from the first sample it receives, so as long
      // as every block arrives, in order, starting from the first, the two
      // streams line up however late the audio connection was established.
      bool audioBacklogged = false;
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (pendingAudio_.size() >= kMaxPendingAudio) {
          audioBacklogged = true;
        } else {
          pendingAudio_.push_back(std::move(tick.audio));
        }
      }
      if (audioBacklogged) {
        // Only reachable if ffmpeg opened the video input and then never opened
        // the audio one. Carrying on would emit a stream whose sound drifts
        // further from its picture every second, so stop and say why.
        fail("ffmpeg never read any audio");
        return;
      }
      queueSignal_.notify_all();
    }
  }

  void audioLoop() {
    if (!audioPipe_.accept()) {
      fail("ffmpeg never connected to the audio socket");
      return;
    }
    audioConnected_ = true;

    for (;;) {
      std::vector<uint8_t> block;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        // Keep going while the video writer might still hand over more: it is
        // what feeds this queue, so an empty queue only means "finished" once
        // that thread has itself finished.
        queueSignal_.wait(lock, [this] {
          return !pendingAudio_.empty() || (stopping_ && videoDrained_);
        });
        if (pendingAudio_.empty()) {
          return;
        }
        block = std::move(pendingAudio_.front());
        pendingAudio_.pop_front();
      }
      if (!block.empty() && !audioPipe_.writeAll(block.data(), block.size())) {
        fail("ffmpeg closed the audio connection");
        return;
      }
      audioSamples_ += static_cast<int64_t>(block.size() /
                                            (sizeof(float) * static_cast<size_t>(
                                                                 settings_.audioChannels)));
    }
  }

  /// Record why the stream stopped and wake the other writer so it can leave
  /// too. Called from either writer thread.
  void fail(const char* reason) {
    if (error_.empty()) {
      error_ = reason;
    }
    running_ = false;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      stopping_ = true;
    }
    queueSignal_.notify_all();
    diag::error("stream '%s': stopped — %s (ffmpeg: %s)", name_.c_str(), reason,
                ffmpeg_.lastLine().c_str());
  }

  OutputSpec spec_;
  StreamSettings settings_;
  VideoFormat format_;
  size_t frameBytes_ = 0;

  OneShotListener videoPipe_;
  OneShotListener audioPipe_;
  FfmpegProcess ffmpeg_;

  std::thread videoWriter_;
  std::thread audioWriter_;
  mutable std::mutex queueMutex_;
  std::condition_variable queueSignal_;
  std::deque<Tick> queue_;
  /// Audio waiting for ffmpeg to open its second input. Bounded, because an
  /// ffmpeg that never opens it must become an error rather than a leak.
  std::deque<std::vector<uint8_t>> pendingAudio_;
  bool stopping_ = false;

  /// Set when the video writer has finished, so the audio writer knows no
  /// further blocks will be handed to it. Guarded by queueMutex_.
  bool videoDrained_ = false;

  std::atomic<bool> videoConnected_{false};
  std::atomic<bool> audioConnected_{false};
  std::atomic<int64_t> frames_{0};
  std::atomic<int64_t> audioSamples_{0};
  std::atomic<int64_t> dropped_{0};
  std::string error_;
  bool channelMismatchLogged_ = false;
};

}  // namespace

std::unique_ptr<IOutput> createStreamOutput(const OutputSpec& spec) {
  return std::make_unique<StreamOutput>(spec);
}

}  // namespace weblinked
