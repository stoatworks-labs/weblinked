// NDI sender.
//
// libndi is opened with dlopen and its flat C entry points resolved by name.
// Two reasons, both practical:
//
//   * NDI's licence does not allow redistributing the library, so it cannot be
//     linked at build time and shipped. Resolving at run time means one binary
//     works whether the operator has the SDK, the redistributable runtime, or
//     neither — in the last case the output simply reports itself unavailable
//     instead of the whole application failing to launch.
//
//   * The flat symbols (NDIlib_send_create and friends) exist in every NDI 5
//     and 6 runtime. The alternative, NDIlib_v6_load(), returns a versioned
//     struct whose type changes between SDK generations, so building against
//     v6 headers would refuse a v5 runtime for no good reason.
//
// Verified against NDI SDK 6.3.2 on macOS arm64: a receiver discovers the
// source and shows correct frames and audio. See docs/04-verification.md.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Keep the NDI structs plain C: the C++ convenience constructors are defined in
// the library, and we are not linking against it.
#define NDILIB_CPP_DEFAULT_CONSTRUCTORS 0
#include <Processing.NDI.Lib.h>

#include "core/dylib.h"
#include "outputs/output.h"

namespace weblinked {
namespace {

#if !defined(NDI_LIBRARY_NAME)
#if defined(__APPLE__)
#define NDI_LIBRARY_NAME "libndi.dylib"
#elif defined(_WIN32)
#define NDI_LIBRARY_NAME "Processing.NDI.Lib.x64.dll"
#else
#define NDI_LIBRARY_NAME "libndi.so.6"
#endif
#endif

/// The entry points we need, and no more.
struct NdiApi {
  bool (*initialize)() = nullptr;
  void (*destroy)() = nullptr;
  const char* (*version)() = nullptr;
  NDIlib_send_instance_t (*send_create)(const NDIlib_send_create_t*) = nullptr;
  void (*send_destroy)(NDIlib_send_instance_t) = nullptr;
  void (*send_send_video_v2)(NDIlib_send_instance_t,
                             const NDIlib_video_frame_v2_t*) = nullptr;
  void (*send_send_audio_v3)(NDIlib_send_instance_t,
                             const NDIlib_audio_frame_v3_t*) = nullptr;
  int (*send_get_no_connections)(NDIlib_send_instance_t, uint32_t) = nullptr;
};

/// One process-wide handle on the library. NDIlib_initialize is reference
/// counted inside the SDK, but opening the dylib repeatedly is pointless and
/// NDIlib_destroy must not be called while another sender is alive.
class NdiRuntime {
 public:
  static NdiRuntime& instance() {
    static NdiRuntime runtime;
    return runtime;
  }

  /// Loads on first use. Returns nullptr and sets `error` if unavailable.
  const NdiApi* acquire(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) {
      if (!ok_) {
        error = error_;
        return nullptr;
      }
      return &api_;
    }
    loaded_ = true;

    std::vector<std::string> candidates;

    // The runtime installer sets this, and honouring it is the documented way
    // to find a redistributable that is not on the default search path.
    const char* runtimeDir = std::getenv(NDILIB_REDIST_FOLDER);
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
      candidates.push_back(std::string(runtimeDir) + "/" + NDI_LIBRARY_NAME);
    }

    // Next to us, then inside a macOS bundle's Frameworks directory.
    for (const auto& dir : Dylib::localSearchPaths()) {
      candidates.push_back(dir + "/" + NDI_LIBRARY_NAME);
    }

    // Then the platform's own search path, then the places the SDK installs to.
    candidates.push_back(NDI_LIBRARY_NAME);
#if defined(__APPLE__)
    candidates.push_back("/Library/NDI SDK for Apple/lib/macOS/" NDI_LIBRARY_NAME);
    candidates.push_back("/usr/local/lib/" NDI_LIBRARY_NAME);
    candidates.push_back("/opt/homebrew/lib/" NDI_LIBRARY_NAME);
#elif !defined(_WIN32)
    candidates.push_back("/usr/local/lib/" NDI_LIBRARY_NAME);
    candidates.push_back("/usr/lib/" NDI_LIBRARY_NAME);
    candidates.push_back("libndi.so.5");
    candidates.push_back("libndi.so");
#endif

    if (!library_.open(candidates)) {
      ok_ = false;
      error_ = "NDI runtime not found (" NDI_LIBRARY_NAME "). Install the NDI "
               "redistributable, or set " NDILIB_REDIST_FOLDER ".";
      error = error_;
      return nullptr;
    }

    const bool resolved =
        library_.symbol("NDIlib_initialize", api_.initialize) &&
        library_.symbol("NDIlib_destroy", api_.destroy) &&
        library_.symbol("NDIlib_send_create", api_.send_create) &&
        library_.symbol("NDIlib_send_destroy", api_.send_destroy) &&
        library_.symbol("NDIlib_send_send_video_v2", api_.send_send_video_v2) &&
        library_.symbol("NDIlib_send_send_audio_v3", api_.send_send_audio_v3);
    // Optional extras: absence degrades reporting, not function.
    library_.symbol("NDIlib_version", api_.version);
    library_.symbol("NDIlib_send_get_no_connections", api_.send_get_no_connections);

    if (!resolved) {
      ok_ = false;
      error_ = "NDI runtime at " + library_.loadedPath() +
               " is missing required entry points (needs NDI 5 or newer)";
      error = error_;
      return nullptr;
    }

    if (!api_.initialize()) {
      // The documented reason is an unsupported CPU.
      ok_ = false;
      error_ = "NDIlib_initialize() failed — this CPU is not supported by the "
               "installed NDI runtime";
      error = error_;
      return nullptr;
    }

    ok_ = true;
    path_ = library_.loadedPath();
    if (api_.version != nullptr) {
      const char* text = api_.version();
      version_ = text != nullptr ? text : "";
    }
    return &api_;
  }

  std::string path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
  }
  std::string version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

 private:
  NdiRuntime() = default;
  // Never destroyed: NDIlib_destroy() during static teardown would race the
  // SDK's own worker threads. The process is exiting anyway.
  ~NdiRuntime() = default;

  mutable std::mutex mutex_;
  Dylib library_;
  NdiApi api_;
  bool loaded_ = false;
  bool ok_ = false;
  std::string error_;
  std::string path_;
  std::string version_;
};

class NdiOutput final : public IOutput {
 public:
  explicit NdiOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "WebLinked" : spec.name),
        // UYVY halves the bytes on the wire compared with BGRA and is what NDI
        // compresses natively. BGRA is only worth it when the page has real
        // transparency to carry.
        alpha_(spec.optionBool("alpha", false)),
        clockVideo_(spec.optionBool("clock_video", false)),
        groups_(spec.optionString("groups")) {}

  ~NdiOutput() override { stop(); }

  std::string kind() const override { return "ndi"; }

  PixelFormat pixelFormat() const override {
    return alpha_ ? PixelFormat::kBGRA : PixelFormat::kUYVY;
  }

  bool start(const VideoFormat& format, std::string& error) override {
    stop();

    api_ = NdiRuntime::instance().acquire(error);
    if (api_ == nullptr) {
      return false;
    }

    format_ = format;

    NDIlib_send_create_t settings;
    std::memset(&settings, 0, sizeof(settings));
    settings.p_ndi_name = name_.c_str();
    settings.p_groups = groups_.empty() ? nullptr : groups_.c_str();
    // We already pace frames ourselves. Letting NDI clock video as well would
    // mean two clocks fighting, and send_video_v2 blocking inside our tick.
    settings.clock_video = clockVideo_;
    settings.clock_audio = false;

    sender_ = api_->send_create(&settings);
    if (sender_ == nullptr) {
      error = "NDIlib_send_create failed for source '" + name_ + "'";
      return false;
    }

    framesSent_ = 0;
    audioFramesSent_ = 0;
    running_ = true;
    return true;
  }

  void stop() override {
    if (sender_ != nullptr && api_ != nullptr) {
      api_->send_destroy(sender_);
    }
    sender_ = nullptr;
    running_ = false;
  }

  void submit(const VideoFrame& video, const AudioBlock& audio) override {
    if (!running_ || sender_ == nullptr) {
      return;
    }

    NDIlib_video_frame_v2_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.xres = video.format().width;
    frame.yres = video.format().height;
    frame.FourCC = alpha_ ? NDIlib_FourCC_video_type_BGRA
                          : NDIlib_FourCC_video_type_UYVY;
    frame.frame_rate_N = video.format().rate.numerator;
    frame.frame_rate_D = video.format().rate.denominator;
    frame.picture_aspect_ratio = static_cast<float>(video.format().aspectRatio());
    frame.frame_format_type = video.format().interlaced
                                  ? NDIlib_frame_format_type_interleaved
                                  : NDIlib_frame_format_type_progressive;
    // Let the SDK synthesise timecode from its own clock; ours is a steady
    // clock with no relationship to time of day.
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.p_data = const_cast<uint8_t*>(video.data());
    frame.line_stride_in_bytes = video.rowBytes();

    // The synchronous send is deliberate. send_send_video_async_v2 returns
    // immediately but keeps borrowing our buffer until the *next* send, which
    // would mean the frame pool could hand the same buffer back to the browser
    // while NDI was still reading it.
    api_->send_send_video_v2(sender_, &frame);
    ++framesSent_;

    if (audio.valid() && audio.planes != nullptr) {
      sendAudio(audio);
    }
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("ndi"));
    value.set("name", json::Value(name_));
    value.set("running", json::Value(running_));
    value.set("pixel_format", json::Value(alpha_ ? "BGRA" : "UYVY"));
    value.set("frames", json::Value(framesSent_));
    value.set("audio_frames", json::Value(audioFramesSent_));
    value.set("library", json::Value(NdiRuntime::instance().path()));
    value.set("sdk_version", json::Value(NdiRuntime::instance().version()));
    if (running_ && api_ != nullptr && api_->send_get_no_connections != nullptr &&
        sender_ != nullptr) {
      // Zero timeout: this is called from the HTTP thread and must not wait.
      value.set("receivers",
                json::Value(api_->send_get_no_connections(sender_, 0)));
    }
    return value;
  }

 private:
  void sendAudio(const AudioBlock& audio) {
    // NDI's planar float layout is one contiguous block with a fixed stride
    // between channels, whereas our planes are separate allocations, so this
    // needs a repack rather than a pointer hand-off.
    const size_t stride = static_cast<size_t>(audio.frames) * sizeof(float);
    audioScratch_.resize(stride * static_cast<size_t>(audio.channels));
    for (int c = 0; c < audio.channels; ++c) {
      std::memcpy(audioScratch_.data() + stride * static_cast<size_t>(c),
                  audio.planes[c], stride);
    }

    NDIlib_audio_frame_v3_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sample_rate = audio.sampleRate;
    frame.no_channels = audio.channels;
    frame.no_samples = audio.frames;
    frame.timecode = NDIlib_send_timecode_synthesize;
    frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
    frame.p_data = audioScratch_.data();
    frame.channel_stride_in_bytes = static_cast<int>(stride);

    api_->send_send_audio_v3(sender_, &frame);
    ++audioFramesSent_;
  }

  const NdiApi* api_ = nullptr;
  NDIlib_send_instance_t sender_ = nullptr;
  VideoFormat format_;
  bool alpha_;
  bool clockVideo_;
  std::string groups_;
  std::vector<uint8_t> audioScratch_;
  int64_t framesSent_ = 0;
  int64_t audioFramesSent_ = 0;
};

}  // namespace

std::unique_ptr<IOutput> createNdiOutput(const OutputSpec& spec) {
  return std::make_unique<NdiOutput>(spec);
}

}  // namespace weblinked
