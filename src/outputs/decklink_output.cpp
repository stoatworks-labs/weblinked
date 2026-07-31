// Blackmagic DeckLink output.
//
// The card is driven with scheduled playback rather than DisplayVideoFrameSync:
// scheduled playback is the only mode that produces genlocked, correctly timed
// SDI, and it is the mode that tolerates the engine's frame clock being a
// software clock. We pre-roll a few frames, start playback, and then top the
// queue up on every tick. The card's own buffer absorbs the difference between
// our clock and its clock; if that difference is one-directional the buffer
// eventually empties or overflows, which is why bufferedFrames appears in
// status() — it is the number to watch.
//
// Ownership follows COM rules: everything obtained from the API is Release()d,
// and the completion callback is reference counted independently of this class.
//
// UNVERIFIED AGAINST HARDWARE. This compiles against DeckLink SDK 12.2 headers
// and the API use follows the SDK's documented scheduled-playback sequence, but
// no DeckLink card has ever been connected to it. See docs/04-verification.md.

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"
#include "core/pixel_convert.h"
#include "outputs/output.h"

namespace weblinked {
namespace {

// The SDK spells the platform differences out in its own headers; these wrap
// the three that reach our code.
#if defined(_WIN32)
using DeckLinkBool = BOOL;
std::string deckLinkStringToStd(BSTR text) {
  if (text == nullptr) {
    return {};
  }
  const int length = ::SysStringLen(text);
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                          nullptr, nullptr);
  std::string out(static_cast<size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr,
                        nullptr);
  ::SysFreeString(text);
  return out;
}
#elif defined(__APPLE__)
using DeckLinkBool = bool;
std::string deckLinkStringToStd(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(text);
  const CFIndex maxBytes =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(maxBytes), '\0');
  std::string result;
  if (CFStringGetCString(text, out.data(), maxBytes, kCFStringEncodingUTF8)) {
    result = out.c_str();
  }
  CFRelease(text);
  return result;
}
#else
using DeckLinkBool = bool;
std::string deckLinkStringToStd(const char* text) {
  if (text == nullptr) {
    return {};
  }
  std::string out(text);
  ::free(const_cast<char*>(text));  // the Linux SDK returns malloc'd strings
  return out;
}
#endif

template <typename T>
void safeRelease(T*& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

/// Notifies us when the card has finished with a frame. We do not recycle
/// frames from here — each scheduled frame owns its own buffer for its whole
/// life — but the completion result is the only place the card tells us it is
/// running late or has run dry, and that is worth surfacing.
class CompletionCallback final : public IDeckLinkVideoOutputCallback {
 public:
  struct Counters {
    std::atomic<int64_t> completed{0};
    std::atomic<int64_t> late{0};
    std::atomic<int64_t> dropped{0};
    std::atomic<int64_t> flushed{0};
  };

  explicit CompletionCallback(Counters* counters) : counters_(counters) {}

  HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame,
                                  BMDOutputFrameCompletionResult result) override {
    ++counters_->completed;
    switch (result) {
      case bmdOutputFrameDisplayedLate:
        ++counters_->late;
        break;
      case bmdOutputFrameDropped:
        ++counters_->dropped;
        break;
      case bmdOutputFrameFlushed:
        ++counters_->flushed;
        break;
      default:
        break;
    }
    // The frame was created by us and handed to ScheduleVideoFrame, which took
    // its own reference; releasing ours here completes the handover.
    if (completedFrame != nullptr) {
      completedFrame->Release();
    }
    return S_OK;
  }

  HRESULT ScheduledPlaybackHasStopped() override { return S_OK; }

  // IUnknown. Hand-rolled because the SDK's objects are COM on every platform.
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refCount_; }
  ULONG Release() override {
    const ULONG count = --refCount_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

 private:
  ~CompletionCallback() override = default;

  Counters* counters_;
  std::atomic<ULONG> refCount_{1};
};

/// What the card should do with the page's alpha channel.
enum class Keying {
  /// Ignore alpha; composite over black and send 4:2:2 YUV.
  kOff,
  /// Fill and key on two SDI connectors, for a downstream keyer or vision
  /// mixer. This is what "key + fill" means on a broadcast card.
  kExternal,
  /// The card composites our frame over its own SDI *input* and sends the
  /// result. No downstream keyer needed, but it costs an input.
  kInternal,
};

Keying parseKeying(const std::string& text) {
  if (text == "external" || text == "keyfill" || text == "key+fill") {
    return Keying::kExternal;
  }
  if (text == "internal") {
    return Keying::kInternal;
  }
  return Keying::kOff;
}

const char* keyingToString(Keying keying) {
  switch (keying) {
    case Keying::kExternal: return "external";
    case Keying::kInternal: return "internal";
    case Keying::kOff:      return "off";
  }
  return "off";
}

class DeckLinkOutput final : public IOutput {
 public:
  explicit DeckLinkOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "decklink" : spec.name),
        deviceIndex_(spec.deviceIndex),
        prerollFrames_(std::max(2, spec.optionInt("preroll", 3))),
        audioChannels_(spec.optionInt("audio_channels", 2)),
        keying_(parseKeying(spec.optionString("keying", "off"))),
        keyLevel_(static_cast<uint8_t>(
            std::clamp(spec.optionInt("key_level", 255), 0, 255))) {}

  ~DeckLinkOutput() override { stop(); }

  std::string kind() const override { return "decklink"; }

  /// 8-bit YUV (UYVY on the wire) is the card's native SDI format; handing it
  /// BGRA would make the driver convert on the CPU behind our back.
  ///
  /// Keying is the exception, and it is not optional: 4:2:2 YUV has nowhere to
  /// put an alpha channel, so a keyed output has to be fed 8-bit BGRA and let
  /// the card derive fill and key from it.
  PixelFormat pixelFormat() const override {
    return keying_ == Keying::kOff ? PixelFormat::kUYVY : PixelFormat::kBGRA;
  }

  bool wantsStraightAlpha() const override { return keying_ != Keying::kOff; }

  BMDPixelFormat cardPixelFormat() const {
    return keying_ == Keying::kOff ? bmdFormat8BitYUV : bmdFormat8BitBGRA;
  }

  bool start(const VideoFormat& format, std::string& error) override {
    stop();
    format_ = format;

    if (!openDevice(error)) {
      stop();
      return false;
    }
    if (!findDisplayMode(format, error)) {
      stop();
      return false;
    }

    if (!configureKeyer(error)) {
      stop();
      return false;
    }

    if (output_->EnableVideoOutput(displayMode_, bmdVideoOutputFlagDefault) != S_OK) {
      error = "EnableVideoOutput failed for " + format.toString() +
              " on '" + deviceName_ + "' (is another application using it?)";
      stop();
      return false;
    }
    videoEnabled_ = true;

    callback_ = new CompletionCallback(&counters_);
    if (output_->SetScheduledFrameCompletionCallback(callback_) != S_OK) {
      error = "SetScheduledFrameCompletionCallback failed";
      stop();
      return false;
    }

    if (audioChannels_ > 0) {
      // 32-bit integer PCM: our source is float, so the wider target avoids
      // throwing away resolution we already have.
      if (output_->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                     bmdAudioSampleType32bitInteger,
                                     static_cast<uint32_t>(audioChannels_),
                                     bmdAudioOutputStreamContinuous) != S_OK) {
        // Not fatal: a video-only feed is better than no feed.
        audioEnabled_ = false;
      } else {
        audioEnabled_ = true;
      }
    }

    // Pre-roll black so playback starts from a defined state rather than from
    // whatever the previous application left in the card's memory.
    for (int i = 0; i < prerollFrames_; ++i) {
      if (!scheduleBlackFrame(error)) {
        stop();
        return false;
      }
    }

    if (output_->StartScheduledPlayback(0, kTimeScale, 1.0) != S_OK) {
      error = "StartScheduledPlayback failed on '" + deviceName_ + "'";
      stop();
      return false;
    }
    playing_ = true;
    running_ = true;
    return true;
  }

  void stop() override {
    if (output_ != nullptr) {
      if (playing_) {
        BMDTimeValue actualStop = 0;
        output_->StopScheduledPlayback(0, &actualStop, kTimeScale);
        playing_ = false;
      }
      output_->SetScheduledFrameCompletionCallback(nullptr);
      if (audioEnabled_) {
        output_->DisableAudioOutput();
        audioEnabled_ = false;
      }
      if (videoEnabled_) {
        output_->DisableVideoOutput();
        videoEnabled_ = false;
      }
    }
    if (keyer_ != nullptr) {
      keyer_->Disable();
    }
    safeRelease(keyer_);
    safeRelease(callback_);
    safeRelease(output_);
    safeRelease(device_);
    running_ = false;
    scheduledFrames_ = 0;
    scheduledAudioFrames_ = 0;
  }

  void submit(const VideoFrame& video, const AudioBlock& audio) override {
    if (!running_ || output_ == nullptr) {
      return;
    }

    IDeckLinkMutableVideoFrame* frame = nullptr;
    if (output_->CreateVideoFrame(format_.width, format_.height,
                                  format_.rowBytes(pixelFormat()),
                                  cardPixelFormat(), bmdFrameFlagDefault,
                                  &frame) != S_OK ||
        frame == nullptr) {
      ++createFailures_;
      return;
    }

    void* bytes = nullptr;
    if (frame->GetBytes(&bytes) == S_OK && bytes != nullptr) {
      const int cardStride = frame->GetRowBytes();
      const int ourStride = video.rowBytes();
      if (cardStride == ourStride) {
        std::memcpy(bytes, video.data(),
                    static_cast<size_t>(ourStride) * format_.height);
      } else {
        // The card is entitled to a wider stride than the picture.
        for (int y = 0; y < format_.height; ++y) {
          std::memcpy(static_cast<uint8_t*>(bytes) + static_cast<size_t>(y) * cardStride,
                      video.data() + static_cast<size_t>(y) * ourStride,
                      static_cast<size_t>(ourStride));
        }
      }
    }

    scheduleFrame(frame);

    if (audioEnabled_ && audio.valid() && audio.interleaved != nullptr) {
      scheduleAudio(audio);
    }
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("decklink"));
    value.set("name", json::Value(name_));
    value.set("running", json::Value(running_));
    value.set("device", json::Value(deviceName_));
    value.set("device_index", json::Value(deviceIndex_));
    value.set("frames", json::Value(scheduledFrames_));
    value.set("audio_frames", json::Value(scheduledAudioFrames_));
    value.set("frames_completed", json::Value(counters_.completed.load()));
    value.set("frames_late", json::Value(counters_.late.load()));
    value.set("frames_dropped", json::Value(counters_.dropped.load()));
    value.set("create_failures", json::Value(createFailures_));
    value.set("keying", json::Value(keyingToString(keying_)));
    if (keying_ != Keying::kOff) {
      value.set("key_level", json::Value(static_cast<int>(keyLevel_)));
      value.set("keyer_active", json::Value(keyer_ != nullptr));
    }

    if (output_ != nullptr) {
      uint32_t buffered = 0;
      if (output_->GetBufferedVideoFrameCount(&buffered) == S_OK) {
        // The number to watch: a steady value means our clock and the card's
        // clock agree. A drift in either direction ends in a glitch.
        value.set("buffered_frames", json::Value(static_cast<int>(buffered)));
      }
      uint32_t bufferedAudio = 0;
      if (audioEnabled_ &&
          output_->GetBufferedAudioSampleFrameCount(&bufferedAudio) == S_OK) {
        value.set("buffered_audio_samples",
                  json::Value(static_cast<int>(bufferedAudio)));
      }
      BMDReferenceStatus reference = static_cast<BMDReferenceStatus>(0);
      if (output_->GetReferenceStatus(&reference) == S_OK) {
        value.set("genlocked",
                  json::Value((reference & bmdReferenceLocked) != 0));
      }
    }
    return value;
  }

 private:
  static constexpr BMDTimeScale kTimeScale = 1'000'000;  // microseconds

  bool openDevice(std::string& error) {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (iterator == nullptr) {
      error = "DeckLink drivers not present (Desktop Video is not installed)";
      return false;
    }

    int index = 0;
    IDeckLink* candidate = nullptr;
    while (iterator->Next(&candidate) == S_OK) {
      if (index == deviceIndex_) {
        device_ = candidate;
        break;
      }
      candidate->Release();
      ++index;
    }
    iterator->Release();

    if (device_ == nullptr) {
      error = "no DeckLink device at index " + std::to_string(deviceIndex_) +
              " (found " + std::to_string(index) + ")";
      return false;
    }

#if defined(_WIN32)
    BSTR nameText = nullptr;
#elif defined(__APPLE__)
    CFStringRef nameText = nullptr;
#else
    const char* nameText = nullptr;
#endif
    if (device_->GetDisplayName(&nameText) == S_OK) {
      deviceName_ = deckLinkStringToStd(nameText);
    }

    if (device_->QueryInterface(IID_IDeckLinkOutput,
                                reinterpret_cast<void**>(&output_)) != S_OK) {
      error = "'" + deviceName_ + "' has no output interface (capture-only card?)";
      return false;
    }
    return true;
  }

  /// Matches our raster and rate against what the card offers.
  ///
  /// Iterating the modes rather than mapping to a bmdMode* constant is
  /// deliberate: the constant list grows with every SDK release, and a card
  /// that supports a mode this build has never heard of should still work.
  bool findDisplayMode(const VideoFormat& format, std::string& error) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
      error = "GetDisplayModeIterator failed";
      return false;
    }

    std::string offered;
    IDeckLinkDisplayMode* mode = nullptr;
    bool found = false;
    while (iterator->Next(&mode) == S_OK) {
      BMDTimeValue duration = 0;
      BMDTimeScale scale = 0;
      mode->GetFrameRate(&duration, &scale);

      const bool sizeMatches = mode->GetWidth() == format.width &&
                               mode->GetHeight() == format.height;
      // Compare as a cross-multiplied rational: duration/scale is the frame
      // period, so scale/duration is the rate.
      const bool rateMatches =
          duration > 0 &&
          static_cast<int64_t>(scale) * format.rate.denominator ==
              static_cast<int64_t>(duration) * format.rate.numerator;

      const BMDFieldDominance dominance = mode->GetFieldDominance();
      const bool modeInterlaced = dominance == bmdUpperFieldFirst ||
                                  dominance == bmdLowerFieldFirst;
      const bool scanMatches = modeInterlaced == format.interlaced;

      if (sizeMatches && rateMatches && scanMatches) {
        displayMode_ = mode->GetDisplayMode();
        found = true;
        mode->Release();
        break;
      }

      if (sizeMatches && !offered.empty()) {
        offered += ", ";
      }
      if (sizeMatches) {
        offered += std::to_string(scale) + "/" + std::to_string(duration);
      }
      mode->Release();
    }
    iterator->Release();

    if (!found) {
      error = "'" + deviceName_ + "' does not support " + format.toString();
      if (!offered.empty()) {
        error += " (it offers these rates at that raster: " + offered + ")";
      }
      return false;
    }

    // Ask the card directly as well: mode enumeration lists what the hardware
    // knows, not what this connection and pixel format can actually carry.
    DeckLinkBool supported = false;
    BMDDisplayMode actualMode = displayMode_;
    if (output_->DoesSupportVideoMode(bmdVideoConnectionUnspecified, displayMode_,
                                      cardPixelFormat(),
                                      bmdNoVideoOutputConversion,
                                      bmdSupportedVideoModeDefault, &actualMode,
                                      &supported) == S_OK &&
        !supported) {
      error = "'" + deviceName_ + "' reports " + format.toString() +
              " as unsupported for " +
              (keying_ == Keying::kOff ? "8-bit YUV" : "8-bit BGRA") + " output";
      if (keying_ != Keying::kOff) {
        // Keying needs an alpha-carrying format, and RGBA costs twice the link
        // bandwidth of the 4:2:2 the same raster fits in. On a DeckLink Duo 2
        // that is the whole difference between 1080p50 and 1080p25: the card
        // reports keying support and every rate as available, then refuses BGRA
        // at the top one. Naming what *would* work turns "unsupported" into a
        // setting to change.
        error += ". Keying carries alpha, which needs about twice the link rate "
                 "of the same raster without it";
        const std::string usable = ratesSupportingAlpha(format);
        if (!usable.empty()) {
          error += "; this card accepts a keyed " +
                   std::to_string(format.width) + "x" +
                   std::to_string(format.height) + " at " + usable;
        }
      }
      return false;
    }
    return true;
  }

  /// Which rates at the *current* raster this card will accept with alpha.
  ///
  /// Asked of the hardware rather than assumed: DoesSupportVideoMode is the only
  /// thing that knows, and the answer differs by model, by profile and by
  /// raster.
  std::string ratesSupportingAlpha(const VideoFormat& format) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_ == nullptr ||
        output_->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
      return {};
    }
    std::string usable;
    IDeckLinkDisplayMode* mode = nullptr;
    while (iterator->Next(&mode) == S_OK) {
      if (mode->GetWidth() == format.width &&
          mode->GetHeight() == format.height) {
        DeckLinkBool ok = false;
        if (output_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                          mode->GetDisplayMode(), bmdFormat8BitBGRA,
                                          bmdNoVideoOutputConversion,
                                          bmdSupportedVideoModeDefault, nullptr,
                                          &ok) == S_OK && ok) {
          BMDTimeValue duration = 0;
          BMDTimeScale scale = 0;
          mode->GetFrameRate(&duration, &scale);
          if (duration > 0) {
            if (!usable.empty()) usable += ", ";
            usable += std::to_string(scale) + "/" + std::to_string(duration);
            if (mode->GetFieldDominance() == bmdUpperFieldFirst ||
                mode->GetFieldDominance() == bmdLowerFieldFirst) {
              usable += " (interlaced)";
            }
          }
        }
      }
      mode->Release();
    }
    iterator->Release();
    return usable;
  }

  /// Turns the card's keyer on, having first checked it has one.
  ///
  /// Worth checking rather than just trying: keying is a hardware capability
  /// that varies by model and, on some cards, by the active profile. Enable()
  /// failing gives no reason, whereas the attribute flag lets us say which of
  /// the two kinds this card lacks — the difference between "buy a different
  /// card" and "switch to internal keying".
  bool configureKeyer(std::string& error) {
    if (keying_ == Keying::kOff) {
      return true;
    }

    IDeckLinkProfileAttributes* attributes = nullptr;
    if (device_->QueryInterface(IID_IDeckLinkProfileAttributes,
                                reinterpret_cast<void**>(&attributes)) == S_OK &&
        attributes != nullptr) {
      const BMDDeckLinkAttributeID attribute =
          keying_ == Keying::kExternal ? BMDDeckLinkSupportsExternalKeying
                                       : BMDDeckLinkSupportsInternalKeying;
      DeckLinkBool supported = false;
      const bool queried = attributes->GetFlag(attribute, &supported) == S_OK;
      attributes->Release();
      if (queried && !supported) {
        error = "'" + deviceName_ + "' does not support " +
                std::string(keyingToString(keying_)) + " keying";
        return false;
      }
    }

    if (device_->QueryInterface(IID_IDeckLinkKeyer,
                                reinterpret_cast<void**>(&keyer_)) != S_OK ||
        keyer_ == nullptr) {
      error = "'" + deviceName_ + "' has no keyer interface";
      return false;
    }

    // Enable(true) is external — fill and key on separate connectors.
    // Enable(false) is internal — composited over the card's own SDI input.
    if (keyer_->Enable(keying_ == Keying::kExternal) != S_OK) {
      error = "could not enable " + std::string(keyingToString(keying_)) +
              " keying on '" + deviceName_ + "'";
      safeRelease(keyer_);
      return false;
    }

    // The keyer's overall opacity, independent of per-pixel alpha. 255 means
    // the page's own alpha decides everything, which is what you want unless
    // you are deliberately fading the whole graphic.
    keyer_->SetLevel(keyLevel_);
    return true;
  }

  bool scheduleBlackFrame(std::string& error) {
    IDeckLinkMutableVideoFrame* frame = nullptr;
    if (output_->CreateVideoFrame(format_.width, format_.height,
                                  format_.rowBytes(pixelFormat()),
                                  cardPixelFormat(), bmdFrameFlagDefault,
                                  &frame) != S_OK ||
        frame == nullptr) {
      error = "CreateVideoFrame failed while pre-rolling";
      return false;
    }
    void* bytes = nullptr;
    if (frame->GetBytes(&bytes) == S_OK && bytes != nullptr) {
      if (keying_ == Keying::kOff) {
        fillBlackUyvy(static_cast<uint8_t*>(bytes), frame->GetRowBytes(),
                      format_.width, format_.height);
      } else {
        // Pre-roll a *transparent* frame when keying: opaque black would punch
        // a black hole in whatever is behind the key for the first few frames.
        std::memset(bytes, 0,
                    static_cast<size_t>(frame->GetRowBytes()) * format_.height);
      }
    }
    scheduleFrame(frame);
    return true;
  }

  /// Schedules at the next free slot on the card's timeline.
  ///
  /// Display times come from a monotonically increasing frame index rather than
  /// from wall-clock arithmetic, so a late tick shortens the queue instead of
  /// scheduling a frame in the past — which the card would reject outright.
  void scheduleFrame(IDeckLinkMutableVideoFrame* frame) {
    const BMDTimeValue duration =
        (static_cast<BMDTimeValue>(kTimeScale) * format_.rate.denominator) /
        format_.rate.numerator;
    const BMDTimeValue displayTime = scheduledFrames_ * duration;

    if (output_->ScheduleVideoFrame(frame, displayTime, duration, kTimeScale) != S_OK) {
      ++scheduleFailures_;
      frame->Release();
      return;
    }
    ++scheduledFrames_;
    // The card holds its own reference; ours is released in the completion
    // callback so the buffer stays alive until the card is finished with it.
  }

  void scheduleAudio(const AudioBlock& audio) {
    const int channels = std::min(audio.channels, audioChannels_);
    audioScratch_.resize(static_cast<size_t>(audio.frames) *
                         static_cast<size_t>(audioChannels_));

    for (int f = 0; f < audio.frames; ++f) {
      for (int c = 0; c < audioChannels_; ++c) {
        float sample = 0.0f;
        if (c < channels) {
          sample = audio.interleaved[static_cast<size_t>(f) * audio.channels + c];
        }
        // Clamp before scaling: a page can and will produce samples outside
        // [-1, 1], and letting those wrap around is the difference between
        // mild distortion and a full-scale click.
        sample = std::clamp(sample, -1.0f, 1.0f);
        audioScratch_[static_cast<size_t>(f) * audioChannels_ + c] =
            static_cast<int32_t>(sample * 2147483647.0f);
      }
    }

    uint32_t written = 0;
    const BMDTimeValue streamTime =
        (scheduledAudioFrames_ * kAudioTimeScale) / audio.sampleRate;
    if (output_->ScheduleAudioSamples(audioScratch_.data(),
                                      static_cast<uint32_t>(audio.frames),
                                      streamTime, kAudioTimeScale,
                                      &written) == S_OK) {
      scheduledAudioFrames_ += written;
    }
  }

  static constexpr BMDTimeScale kAudioTimeScale = 48'000;

  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  IDeckLinkKeyer* keyer_ = nullptr;
  CompletionCallback* callback_ = nullptr;
  CompletionCallback::Counters counters_;

  VideoFormat format_;
  BMDDisplayMode displayMode_ = bmdModeHD1080p50;
  std::string deviceName_;
  int deviceIndex_;
  int prerollFrames_;
  int audioChannels_;
  Keying keying_;
  uint8_t keyLevel_;
  bool videoEnabled_ = false;
  bool audioEnabled_ = false;
  bool playing_ = false;

  int64_t scheduledFrames_ = 0;
  int64_t scheduledAudioFrames_ = 0;
  int64_t createFailures_ = 0;
  int64_t scheduleFailures_ = 0;
  std::vector<int32_t> audioScratch_;
};

}  // namespace

std::unique_ptr<IOutput> createDeckLinkOutput(const OutputSpec& spec) {
  return std::make_unique<DeckLinkOutput>(spec);
}

}  // namespace weblinked
