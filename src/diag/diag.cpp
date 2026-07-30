#include "diag/diag.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <process.h>
#else
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace weblinked::diag {
namespace {

constexpr const char* kSchema = "stoatworks.diagnostics/1";
constexpr size_t kLogTailLines = 400;

struct State {
  std::mutex mutex;
  Options options;
  bool initialised = false;
  Level level = Level::kInfo;
  std::filesystem::path directory;
  std::filesystem::path logPath;
  std::ofstream stream;
  size_t written = 0;
  /// Kept in memory so a crash report can carry the run-up to the failure even
  /// if the log file was rotated a moment ago.
  std::deque<std::string> tail;
  json::Value config = json::Value::object();
};

State& state() {
  static State instance;
  return instance;
}

std::string timestamp() {
  const auto now = std::time(nullptr);
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

std::string fileTimestamp() {
  const auto now = std::time(nullptr);
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
  return buffer;
}

std::filesystem::path defaultLogDirectory(const std::string& appName) {
#if defined(_WIN32)
  const char* base = std::getenv("LOCALAPPDATA");
  std::filesystem::path root =
      base != nullptr ? std::filesystem::path(base) : std::filesystem::temp_directory_path();
  return root / appName / "logs";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  std::filesystem::path root =
      home != nullptr ? std::filesystem::path(home) : std::filesystem::temp_directory_path();
  return root / "Library" / "Logs" / appName;
#else
  if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
    return std::filesystem::path(xdg) / appName / "logs";
  }
  const char* home = std::getenv("HOME");
  std::filesystem::path root =
      home != nullptr ? std::filesystem::path(home) : std::filesystem::temp_directory_path();
  return root / ".local" / "state" / appName / "logs";
#endif
}

int processId() {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

std::string platformName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::string architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return "unknown";
#endif
}

std::string osVersion() {
#if defined(__APPLE__)
  char buffer[256] = {};
  size_t size = sizeof(buffer);
  if (sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) == 0) {
    return buffer;
  }
  return "unknown";
#elif defined(_WIN32)
  // GetVersionEx lies for compatibility; the registry value does not.
  DWORD size = 0;
  char build[64] = {};
  DWORD length = sizeof(build);
  if (RegGetValueA(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                   "CurrentBuild", RRF_RT_REG_SZ, nullptr, build,
                   &length) == ERROR_SUCCESS) {
    (void)size;
    return std::string("build ") + build;
  }
  return "unknown";
#else
  std::ifstream release("/etc/os-release");
  std::string line;
  while (std::getline(release, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string value = line.substr(12);
      if (!value.empty() && value.front() == '"') {
        value = value.substr(1, value.size() - 2);
      }
      return value;
    }
  }
  return "unknown";
#endif
}

/// Redacts values whose *key* suggests a secret. Applied once, when the config
/// is handed over, so nothing downstream has to remember to do it.
json::Value redact(const json::Value& value) {
  static const char* kSuspicious[] = {"password", "secret", "token", "key",
                                      "credential", "auth", "cookie"};

  if (value.isObject()) {
    json::Value out = json::Value::object();
    for (const auto& [key, child] : value.members()) {
      std::string lowered = key;
      for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      bool suspicious = false;
      for (const char* needle : kSuspicious) {
        if (lowered.find(needle) != std::string::npos) {
          suspicious = true;
          break;
        }
      }
      out.set(key, suspicious ? json::Value("[redacted]") : redact(child));
    }
    return out;
  }
  if (value.isArray()) {
    json::Value out = json::Value::array();
    for (const auto& element : value.elements()) {
      out.push(redact(element));
    }
    return out;
  }
  return value;
}

std::vector<std::string> backtraceFrames() {
  std::vector<std::string> frames;
#if defined(_WIN32)
  void* addresses[62] = {};
  const USHORT captured = CaptureStackBackTrace(0, 62, addresses, nullptr);
  HANDLE process = GetCurrentProcess();
  SymInitialize(process, nullptr, TRUE);
  alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  for (USHORT i = 0; i < captured; ++i) {
    DWORD64 displacement = 0;
    if (SymFromAddr(process, reinterpret_cast<DWORD64>(addresses[i]),
                    &displacement, symbol)) {
      frames.emplace_back(symbol->Name);
    } else {
      char text[32];
      std::snprintf(text, sizeof(text), "0x%llx",
                    reinterpret_cast<unsigned long long>(addresses[i]));
      frames.emplace_back(text);
    }
  }
#else
  void* addresses[64] = {};
  const int captured = ::backtrace(addresses, 64);
  char** symbols = ::backtrace_symbols(addresses, captured);
  if (symbols != nullptr) {
    for (int i = 0; i < captured; ++i) {
      frames.emplace_back(symbols[i]);
    }
    ::free(symbols);
  }
#endif
  return frames;
}

json::Value buildAppInfo() {
  auto& s = state();
  json::Value app = json::Value::object();
  app.set("name", json::Value(s.options.appName));
  app.set("version", json::Value(s.options.version));
  app.set("pid", json::Value(processId()));
  app.set("build_date", json::Value(__DATE__ " " __TIME__));
  return app;
}

json::Value buildPlatformInfo() {
  json::Value platform = json::Value::object();
  platform.set("os", json::Value(platformName()));
  platform.set("os_version", json::Value(osVersion()));
  platform.set("arch", json::Value(architecture()));
  return platform;
}

/// Assumes the caller holds the lock.
void rotateIfNeededLocked() {
  auto& s = state();
  if (s.written < s.options.maxLogBytes) {
    return;
  }
  s.stream.close();
  std::error_code ec;
  const auto previous = s.logPath.string() + ".1";
  std::filesystem::remove(previous, ec);
  std::filesystem::rename(s.logPath, previous, ec);
  s.stream.open(s.logPath, std::ios::out | std::ios::trunc);
  s.written = 0;
}

void writeLine(Level level, const std::string& message) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (level < s.level) {
    return;
  }

  std::ostringstream line;
  line << timestamp() << ' ' << levelToString(level) << ' ' << message;
  const std::string text = line.str();

  s.tail.push_back(text);
  while (s.tail.size() > kLogTailLines) {
    s.tail.pop_front();
  }

  if (s.stream.is_open()) {
    s.stream << text << '\n';
    s.stream.flush();  // a crash must not lose the line that explains it
    s.written += text.size() + 1;
    rotateIfNeededLocked();
  }

  // Errors also go to stderr, which is where an operator running from a terminal
  // is looking.
  if (level >= Level::kWarn) {
    std::fprintf(stderr, "%s\n", text.c_str());
  }
}

std::string format(const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  const int length = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (length <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(length), '\0');
  std::vsnprintf(out.data(), static_cast<size_t>(length) + 1, fmt, args);
  return out;
}

#if !defined(_WIN32)
void signalHandler(int signum) {
  // Not strictly async-signal-safe — allocating and writing a file in a handler
  // is against the rules. It is done anyway, deliberately: the process is
  // already dying, and a report that usually arrives is worth more than a
  // guarantee of never deadlocking on the way out. The handler is reset to the
  // default first so a fault inside it still terminates.
  std::signal(signum, SIG_DFL);
  const char* name = "signal";
  switch (signum) {
    case SIGSEGV: name = "SIGSEGV (invalid memory access)"; break;
    case SIGABRT: name = "SIGABRT (abort)"; break;
    case SIGBUS:  name = "SIGBUS (bus error)"; break;
    case SIGILL:  name = "SIGILL (illegal instruction)"; break;
    case SIGFPE:  name = "SIGFPE (arithmetic error)"; break;
    default: break;
  }
  writeReport(name);
  std::raise(signum);
}
#else
LONG WINAPI exceptionHandler(EXCEPTION_POINTERS* info) {
  char reason[64];
  std::snprintf(reason, sizeof(reason), "exception 0x%08lx",
                info != nullptr ? info->ExceptionRecord->ExceptionCode : 0);
  writeReport(reason);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

const char* levelToString(Level level) {
  switch (level) {
    case Level::kTrace: return "TRACE";
    case Level::kDebug: return "DEBUG";
    case Level::kInfo:  return "INFO ";
    case Level::kWarn:  return "WARN ";
    case Level::kError: return "ERROR";
    case Level::kFatal: return "FATAL";
  }
  return "INFO ";
}

Level levelFromString(const std::string& text) {
  std::string lowered = text;
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lowered == "trace") return Level::kTrace;
  if (lowered == "debug") return Level::kDebug;
  if (lowered == "warn" || lowered == "warning") return Level::kWarn;
  if (lowered == "error") return Level::kError;
  if (lowered == "fatal") return Level::kFatal;
  return Level::kInfo;
}

void init(const Options& options) {
  auto& s = state();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.initialised) {
      return;
    }
    s.initialised = true;
    s.options = options;
    s.level = options.defaultLevel;

    // Environment overrides, so a support call can raise the log level without a
    // new build.
    const std::string levelVar = options.envPrefix + "_LOG";
    if (const char* value = std::getenv(levelVar.c_str())) {
      s.level = levelFromString(value);
    }
    const std::string dirVar = options.envPrefix + "_LOG_DIR";
    if (const char* value = std::getenv(dirVar.c_str())) {
      s.directory = value;
    } else {
      s.directory = defaultLogDirectory(options.appName);
    }

    std::error_code ec;
    std::filesystem::create_directories(s.directory, ec);
    s.logPath = s.directory / (options.appName + ".log");
    s.stream.open(s.logPath, std::ios::out | std::ios::app);
    if (s.stream.is_open()) {
      s.written = static_cast<size_t>(std::filesystem::file_size(s.logPath, ec));
    }
  }

  info("%s %s starting on %s %s (%s)", options.appName.c_str(),
       options.version.c_str(), platformName().c_str(), osVersion().c_str(),
       architecture().c_str());

  if (options.installCrashHandler) {
    installCrashHandler();
  }
}

void installCrashHandler() {
#if defined(_WIN32)
  SetUnhandledExceptionFilter(exceptionHandler);
#else
  for (int signum : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
    std::signal(signum, signalHandler);
  }
#endif
}

void shutdown() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.stream.is_open()) {
    s.stream.flush();
    s.stream.close();
  }
  s.initialised = false;
}

void setConfig(const json::Value& config) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.config = redact(config);
}

void log(Level level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string message = format(fmt, args);
  va_end(args);
  writeLine(level, message);
}

#define WEBLINKED_DIAG_LEVEL_FN(name, level)         \
  void name(const char* fmt, ...) {                  \
    va_list args;                                    \
    va_start(args, fmt);                             \
    const std::string message = format(fmt, args);   \
    va_end(args);                                    \
    writeLine(level, message);                       \
  }

WEBLINKED_DIAG_LEVEL_FN(trace, Level::kTrace)
WEBLINKED_DIAG_LEVEL_FN(debug, Level::kDebug)
WEBLINKED_DIAG_LEVEL_FN(info, Level::kInfo)
WEBLINKED_DIAG_LEVEL_FN(warn, Level::kWarn)
WEBLINKED_DIAG_LEVEL_FN(error, Level::kError)

#undef WEBLINKED_DIAG_LEVEL_FN

std::string logDirectory() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.directory.string();
}

std::string logFilePath() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.logPath.string();
}

std::string writeReport(const std::string& reason) {
  // Gather the backtrace before taking the lock: if we are here from a signal
  // handler, the lock may already be held by the thread that faulted.
  const std::vector<std::string> frames = backtraceFrames();

  auto& s = state();
  json::Value report = json::Value::object();
  report.set("schema", json::Value(kSchema));
  report.set("kind", json::Value("crash-report"));
  report.set("generated_at", json::Value(timestamp()));
  report.set("reason", json::Value(reason));

  json::Value backtrace = json::Value::array();
  for (const auto& frame : frames) {
    backtrace.push(json::Value(frame));
  }
  report.set("backtrace", backtrace);

  std::filesystem::path path;
  {
    // try_lock rather than lock: a deadlock here would turn a crash report into
    // a hang, which is strictly worse.
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    report.set("app", buildAppInfo());
    report.set("platform", buildPlatformInfo());
    report.set("config", s.config);

    json::Value tail = json::Value::array();
    for (const auto& line : s.tail) {
      tail.push(json::Value(line));
    }
    report.set("log_tail", tail);

    path = s.directory / (s.options.appName + "-crash-" + fileTimestamp() + ".json");
  }

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (out.is_open()) {
    out << report.serialize(true) << '\n';
  }
  return path.string();
}

std::string collectBundle() {
  auto& s = state();

  json::Value bundle = json::Value::object();
  bundle.set("schema", json::Value(kSchema));
  bundle.set("kind", json::Value("diagnostics-bundle"));
  bundle.set("generated_at", json::Value(timestamp()));

  std::filesystem::path path;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    bundle.set("app", buildAppInfo());
    bundle.set("platform", buildPlatformInfo());
    bundle.set("config", s.config);
    bundle.set("log_directory", json::Value(s.directory.string()));

    json::Value tail = json::Value::array();
    for (const auto& line : s.tail) {
      tail.push(json::Value(line));
    }
    bundle.set("log_tail", tail);

    // Any crash reports sitting alongside the log, so one file is genuinely
    // enough to send.
    json::Value crashes = json::Value::array();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(s.directory, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.find("-crash-") != std::string::npos) {
        crashes.push(json::Value(name));
      }
    }
    bundle.set("crash_reports", crashes);

    path = s.directory / (s.options.appName + "-diagnostics-" + fileTimestamp() + ".json");
  }

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (out.is_open()) {
    out << bundle.serialize(true) << '\n';
  }
  return path.string();
}

}  // namespace weblinked::diag
