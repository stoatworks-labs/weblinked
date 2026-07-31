// Open Media Transport sender.
//
// OMT is the open, royalty-free alternative to NDI (MIT licensed, from the vMix
// people). Its reference implementation is .NET — libomtnet — with libomt as a
// C wrapper around it, so what we link against is a shared library whose
// published builds are:
//
//   Windows x64, Windows arm64, macOS arm64 (macOS 15 or newer)
//
// There is no published Linux binary. A Linux build has to compile libomtnet
// from source, which is why this backend is loaded at run time like NDI rather
// than linked: on a platform with no library, the output reports itself
// unavailable and everything else keeps working.
//
// Video encoding needs libvmx (the VMX codec) beside libomt. libomt loads it
// itself, so it must be in the same directory or on the library search path.
//
// NOT verified against a real receiver: this was written against libomt.h from
// the v1.0.0.16 binary release and compiles against it, but no OMT receiver has
// ever consumed its output. See docs/04-verification.md.

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <libomt.h>

#include "core/dylib.h"
#include "outputs/output.h"

namespace weblinked {
namespace {

#if !defined(OMT_LIBRARY_NAME)
#if defined(__APPLE__)
#define OMT_LIBRARY_NAME "libomt.dylib"
#elif defined(_WIN32)
#define OMT_LIBRARY_NAME "libomt.dll"
#else
#define OMT_LIBRARY_NAME "libomt.so"
#endif
#endif

struct OmtApi {
  omt_send_t* (*send_create)(const char*, OMTQuality) = nullptr;
  void (*send_destroy)(omt_send_t*) = nullptr;
  int (*send)(omt_send_t*, OMTMediaFrame*) = nullptr;
  int (*send_connections)(omt_send_t*) = nullptr;
  void (*send_setsenderinformation)(omt_send_t*, OMTSenderInfo*) = nullptr;
  void (*send_getvideostatistics)(omt_send_t*, OMTStatistics*) = nullptr;
  int (*send_gettally)(omt_send_t*, int, OMTTally*) = nullptr;
};

class OmtRuntime {
 public:
  static OmtRuntime& instance() {
    static OmtRuntime runtime;
    return runtime;
  }

  const OmtApi* acquire(std::string& error) {
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
    for (const auto& dir : Dylib::localSearchPaths()) {
      candidates.push_back(dir + "/" + OMT_LIBRARY_NAME);
    }
    candidates.push_back(OMT_LIBRARY_NAME);
#if defined(__APPLE__)
    candidates.push_back("/Library/OMT/" OMT_LIBRARY_NAME);
    candidates.push_back("/usr/local/lib/" OMT_LIBRARY_NAME);
#elif !defined(_WIN32)
    candidates.push_back("/usr/local/lib/" OMT_LIBRARY_NAME);
    candidates.push_back("/usr/lib/" OMT_LIBRARY_NAME);
#endif

    if (!library_.open(candidates)) {
      ok_ = false;
      error_ = "OMT library not found (" OMT_LIBRARY_NAME "). Download the "
               "binaries from github.com/openmediatransport/libomtnet/releases "
               "and put libomt and libvmx beside the application."
#if !defined(__APPLE__) && !defined(_WIN32)
               " Note that OMT publishes no Linux binaries; libomtnet must be "
               "built from source."
#endif
          ;
      error = error_;
      return nullptr;
    }

    const bool resolved =
        library_.symbol("omt_send_create", api_.send_create) &&
        library_.symbol("omt_send_destroy", api_.send_destroy) &&
        library_.symbol("omt_send", api_.send);
    library_.symbol("omt_send_connections", api_.send_connections);
    library_.symbol("omt_send_setsenderinformation", api_.send_setsenderinformation);
    library_.symbol("omt_send_getvideostatistics", api_.send_getvideostatistics);
    library_.symbol("omt_send_gettally", api_.send_gettally);

    if (!resolved) {
      ok_ = false;
      error_ = "OMT library at " + library_.loadedPath() +
               " is missing required entry points";
      error = error_;
      return nullptr;
    }

    ok_ = true;
    path_ = library_.loadedPath();
    return &api_;
  }

  std::string path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
  }

 private:
  OmtRuntime() = default;
  ~OmtRuntime() = default;

  mutable std::mutex mutex_;
  Dylib library_;
  OmtApi api_;
  bool loaded_ = false;
  bool ok_ = false;
  std::string error_;
  std::string path_;
};

OMTQuality parseQuality(const std::string& text) {
  if (text == "low") return OMTQuality_Low;
  if (text == "medium") return OMTQuality_Medium;
  if (text == "high") return OMTQuality_High;
  return OMTQuality_Default;
}

class OmtOutput final : public IOutput {
 public:
  explicit OmtOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "WebLinked" : spec.name),
        alpha_(spec.optionBool("alpha", false)),
        quality_(parseQuality(spec.optionString("quality", "default"))) {}

  ~OmtOutput() override { stop(); }

  std::string kind() const override { return "omt"; }

  PixelFormat pixelFormat() const override {
    return alpha_ ? PixelFormat::kBGRA : PixelFormat::kUYVY;
  }

  bool wantsStraightAlpha() const override { return alpha_; }

  bool start(const VideoFormat& format, std::string& error) override {
    stop();

    api_ = OmtRuntime::instance().acquire(error);
    if (api_ == nullptr) {
      return false;
    }

    format_ = format;
    sender_ = api_->send_create(name_.c_str(), quality_);
    if (sender_ == nullptr) {
      error = "omt_send_create failed for source '" + name_ + "'";
      return false;
    }

    if (api_->send_setsenderinformation != nullptr) {
      OMTSenderInfo info;
      std::memset(&info, 0, sizeof(info));
      std::snprintf(info.ProductName, sizeof(info.ProductName), "WebLinked");
      std::snprintf(info.Manufacturer, sizeof(info.Manufacturer), "stoatworks");
      std::snprintf(info.Version, sizeof(info.Version), "%s", WEBLINKED_VERSION);
      api_->send_setsenderinformation(sender_, &info);
    }

    framesSent_ = 0;
    audioFramesSent_ = 0;
    tick_ = 0;
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

    // OMT timestamps are 100 ns units. Derived from the tick index rather than
    // accumulated, for the same reason FrameClock computes deadlines that way.
    const int64_t timestamp =
        (tick_ * 10'000'000LL * format_.rate.denominator) / format_.rate.numerator;

    OMTMediaFrame frame = {};  // the header is explicit that this must be zeroed
    frame.Type = OMTFrameType_Video;
    frame.Timestamp = timestamp;
    frame.Codec = alpha_ ? OMTCodec_BGRA : OMTCodec_UYVY;
    frame.Width = video.format().width;
    frame.Height = video.format().height;
    frame.Stride = video.rowBytes();
    frame.Flags = OMTVideoFlags_None;
    if (alpha_) {
      // Without this flag libomt treats BGRA as BGRX and discards the alpha.
      frame.Flags = static_cast<OMTVideoFlags>(frame.Flags | OMTVideoFlags_Alpha);
    }
    if (video.format().interlaced) {
      frame.Flags =
          static_cast<OMTVideoFlags>(frame.Flags | OMTVideoFlags_Interlaced);
    }
    frame.FrameRateN = video.format().rate.numerator;
    frame.FrameRateD = video.format().rate.denominator;
    frame.AspectRatio = static_cast<float>(video.format().aspectRatio());
    frame.ColorSpace = video.format().height >= 720 ? OMTColorSpace_BT709
                                                    : OMTColorSpace_BT601;
    frame.Data = const_cast<uint8_t*>(video.data());
    frame.DataLength = static_cast<int>(video.rowBytes() * video.format().height);

    if (api_->send(sender_, &frame) > 0) {
      ++framesSent_;
    }

    if (audio.valid() && audio.planes != nullptr) {
      sendAudio(audio, timestamp);
    }
    ++tick_;
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("omt"));
    value.set("name", json::Value(name_));
    value.set("running", json::Value(running_));
    value.set("pixel_format", json::Value(alpha_ ? "BGRA" : "UYVY"));
    value.set("frames", json::Value(framesSent_));
    value.set("audio_frames", json::Value(audioFramesSent_));
    value.set("library", json::Value(OmtRuntime::instance().path()));
    if (running_ && api_ != nullptr && sender_ != nullptr) {
      if (api_->send_connections != nullptr) {
        value.set("receivers", json::Value(api_->send_connections(sender_)));
      }
      if (api_->send_gettally != nullptr) {
        OMTTally tally = {};
        // Zero timeout: called from the HTTP thread.
        if (api_->send_gettally(sender_, 0, &tally) != 0) {
          json::Value tallyValue = json::Value::object();
          tallyValue.set("preview", json::Value(tally.preview != 0));
          tallyValue.set("program", json::Value(tally.program != 0));
          value.set("tally", tallyValue);
        }
      }
    }
    return value;
  }

 private:
  void sendAudio(const AudioBlock& audio, int64_t timestamp) {
    // FPA1 is planar float in one contiguous block, channel after channel.
    const size_t stride = static_cast<size_t>(audio.frames) * sizeof(float);
    audioScratch_.resize(stride * static_cast<size_t>(audio.channels));
    for (int c = 0; c < audio.channels; ++c) {
      std::memcpy(audioScratch_.data() + stride * static_cast<size_t>(c),
                  audio.planes[c], stride);
    }

    OMTMediaFrame frame = {};
    frame.Type = OMTFrameType_Audio;
    frame.Timestamp = timestamp;
    frame.Codec = OMTCodec_FPA1;
    frame.SampleRate = audio.sampleRate;
    frame.Channels = audio.channels;
    frame.SamplesPerChannel = audio.frames;
    frame.Data = audioScratch_.data();
    frame.DataLength = static_cast<int>(audioScratch_.size());

    if (api_->send(sender_, &frame) > 0) {
      ++audioFramesSent_;
    }
  }

  const OmtApi* api_ = nullptr;
  omt_send_t* sender_ = nullptr;
  VideoFormat format_;
  bool alpha_;
  OMTQuality quality_;
  std::vector<uint8_t> audioScratch_;
  int64_t framesSent_ = 0;
  int64_t audioFramesSent_ = 0;
  int64_t tick_ = 0;
};

}  // namespace

std::unique_ptr<IOutput> createOmtOutput(const OutputSpec& spec) {
  return std::make_unique<OmtOutput>(spec);
}

}  // namespace weblinked
