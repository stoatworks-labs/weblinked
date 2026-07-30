// AJA Kona / Io / Corvid output, via the NTV2 SDK's AutoCirculate mechanism.
//
// AutoCirculate is AJA's equivalent of DeckLink's scheduled playback: the driver
// owns a ring of frame buffers in the card's SDRAM and clocks them out on the
// card's own vertical interval. We top that ring up on each engine tick and
// AutoCirculate handles the timing. The number to watch is the buffer level in
// status(): steady means our clock and the card's clock agree.
//
// Unlike DeckLink, this is a static link against libajantv2 (MIT licensed, from
// github.com/aja-video/libajantv2), which is how AJA intends the SDK to be
// consumed. The consequence is that this backend cannot soft-fail at run time
// the way NDI does — a build either has it or does not, hence
// WEBLINKED_WITH_AJA defaulting to OFF.
//
// Requires libajantv2 18.x: SetTaskMode and SetSDIOutputAudioEnabled replaced
// the older spellings in SDK 18.0 and 17.5 respectively.
//
// UNVERIFIED AGAINST HARDWARE. Compiles against libajantv2 18.1.0 and follows
// the SDK's documented playout sequence, but no AJA card has ever been
// connected to it. See docs/04-verification.md.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "ajabase/system/process.h"
#include "ajantv2/includes/ntv2card.h"
#include "ajantv2/includes/ntv2devicescanner.h"
#include "ajantv2/includes/ntv2enums.h"
#include "ajantv2/includes/ntv2utils.h"

#include "outputs/output.h"

namespace weblinked {
namespace {

/// Identifies us to the driver so the AJA Control Panel shows who has the card.
/// The convention is a four-character code packed into a ULWord.
constexpr ULWord kAppSignature = AJA_FOURCC('W', 'b', 'L', 'k');

/// Maps our exact rational rate onto an NTV2FrameRate.
///
/// Enumerated rather than table-mapped: the SDK's own conversion is the
/// authority on which enum means 60000/1001, and a hand-written table would go
/// stale the next time AJA adds a rate.
NTV2FrameRate ntv2FrameRateFor(const FrameRate& rate) {
  for (int candidate = NTV2_FRAMERATE_UNKNOWN + 1; candidate < NTV2_NUM_FRAMERATES;
       ++candidate) {
    const auto ntv2Rate = static_cast<NTV2FrameRate>(candidate);
    ULWord numerator = 0;
    ULWord denominator = 0;
    if (!GetFramesPerSecond(ntv2Rate, numerator, denominator) || denominator == 0) {
      continue;
    }
    // Cross-multiply so 60000/1001 matches 60/1.001 regardless of how the SDK
    // chose to express it.
    if (static_cast<int64_t>(numerator) * rate.denominator ==
        static_cast<int64_t>(denominator) * rate.numerator) {
      return ntv2Rate;
    }
  }
  return NTV2_FRAMERATE_UNKNOWN;
}

class AjaOutput final : public IOutput {
 public:
  explicit AjaOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "aja" : spec.name),
        deviceIndex_(spec.deviceIndex),
        channel_(static_cast<NTV2Channel>(
            std::clamp(spec.optionInt("channel", 1), 1, 8) - 1)),
        ringFrames_(std::clamp(spec.optionInt("ring_frames", 7), 2, 16)),
        audioChannels_(spec.optionInt("audio_channels", 2)) {}

  ~AjaOutput() override { stop(); }

  std::string kind() const override { return "aja"; }

  /// 8-bit YCbCr is the card's native SDI layout, same reasoning as DeckLink.
  PixelFormat pixelFormat() const override { return PixelFormat::kUYVY; }

  bool start(const VideoFormat& format, std::string& error) override {
    stop();
    format_ = format;

    if (!CNTV2DeviceScanner::GetDeviceAtIndex(static_cast<ULWord>(deviceIndex_),
                                              device_)) {
      error = "no AJA device at index " + std::to_string(deviceIndex_) +
              " (is the driver installed?)";
      return false;
    }
    if (!device_.IsDeviceReady(false)) {
      error = "AJA device " + std::to_string(deviceIndex_) + " is not ready";
      return false;
    }
    deviceName_ = device_.GetDisplayName();

    const NTV2FrameRate ntv2Rate = ntv2FrameRateFor(format.rate);
    if (ntv2Rate == NTV2_FRAMERATE_UNKNOWN) {
      error = "AJA has no frame rate matching " + format.rate.toString();
      return false;
    }

    videoFormat_ = GetFirstMatchingVideoFormat(
        ntv2Rate, static_cast<UWord>(format.height),
        static_cast<UWord>(format.width), format.interlaced,
        /*inIsLevelB=*/false, /*inIsPSF=*/false);
    if (videoFormat_ == NTV2_FORMAT_UNKNOWN) {
      error = "AJA has no video format matching " + format.toString();
      return false;
    }
    if (!NTV2DeviceCanDoVideoFormat(device_.GetDeviceID(), videoFormat_)) {
      error = "'" + deviceName_ + "' does not support " + format.toString();
      return false;
    }

    // Take the card before configuring it, so the Control Panel and any other
    // NTV2 application know it is in use.
    device_.AcquireStreamForApplicationWithReference(
        kAppSignature, static_cast<int32_t>(AJAProcess::GetPid()));
    acquired_ = true;

    // OEM task mode: we configure the device, the driver stays out of the way.
    device_.SetTaskMode(NTV2_OEM_TASKS);

    device_.EnableChannel(channel_);
    device_.SetMode(channel_, NTV2_MODE_DISPLAY);
    device_.SetVideoFormat(videoFormat_, false, false, channel_);
    if (!device_.SetFrameBufferFormat(channel_, NTV2_FBF_8BIT_YCBCR)) {
      error = "SetFrameBufferFormat(8-bit YCbCr) failed on '" + deviceName_ + "'";
      stop();
      return false;
    }
    // Free-run: nothing upstream is providing reference, and the alternative is
    // a card that refuses to output until it sees genlock it will never get.
    device_.SetReference(NTV2_REFERENCE_FREERUN);

    // Route the frame store to the matching SDI output.
    device_.Connect(GetSDIOutputInputXpt(channel_),
                    GetFrameStoreOutputXptFromChannel(channel_));
    device_.SetSDIOutputStandard(static_cast<UWord>(channel_),
                                 GetNTV2StandardFromVideoFormat(videoFormat_));

    NTV2AudioSystem audioSystem = NTV2_AUDIOSYSTEM_INVALID;
    if (audioChannels_ > 0 &&
        NTV2DeviceGetNumAudioSystems(device_.GetDeviceID()) > 0) {
      audioSystem = NTV2ChannelToAudioSystem(channel_);
      device_.SetNumberAudioChannels(static_cast<ULWord>(audioChannels_),
                                     audioSystem);
      device_.SetAudioRate(NTV2_AUDIO_48K, audioSystem);
      device_.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, audioSystem);
      device_.SetSDIOutputAudioSystem(channel_, audioSystem);
      device_.SetSDIOutputAudioEnabled(channel_, true);
      audioSystem_ = audioSystem;
    }

    // Signal routing must be complete before this call — the SDK is explicit.
    device_.AutoCirculateStop(channel_);
    if (!device_.AutoCirculateInitForOutput(
            channel_, static_cast<UWord>(ringFrames_), audioSystem)) {
      error = "AutoCirculateInitForOutput failed on '" + deviceName_ +
              "' (are the frame buffers already claimed?)";
      stop();
      return false;
    }
    if (!device_.AutoCirculateStart(channel_)) {
      error = "AutoCirculateStart failed on '" + deviceName_ + "'";
      stop();
      return false;
    }

    circulating_ = true;
    framesSent_ = 0;
    framesDropped_ = 0;
    running_ = true;
    return true;
  }

  void stop() override {
    if (circulating_) {
      device_.AutoCirculateStop(channel_);
      circulating_ = false;
    }
    if (acquired_) {
      device_.ReleaseStreamForApplicationWithReference(
          kAppSignature, static_cast<int32_t>(AJAProcess::GetPid()));
      acquired_ = false;
    }
    audioSystem_ = NTV2_AUDIOSYSTEM_INVALID;
    running_ = false;
  }

  void submit(const VideoFrame& video, const AudioBlock& audio) override {
    if (!running_ || !circulating_) {
      return;
    }

    AUTOCIRCULATE_STATUS status;
    if (!device_.AutoCirculateGetStatus(channel_, status)) {
      return;
    }
    bufferLevel_ = static_cast<int>(status.GetBufferLevel());
    if (!status.CanAcceptMoreOutputFrames()) {
      // The card is still busy with what it has. Dropping here is correct:
      // blocking would hold up every other output on this tick.
      ++framesDropped_;
      return;
    }

    AUTOCIRCULATE_TRANSFER transfer;
    // The SDK's buffer setters take ULWord* purely for historical reasons; the
    // buffer is bytes and the count is in bytes.
    transfer.SetVideoBuffer(
        reinterpret_cast<ULWord*>(const_cast<uint8_t*>(video.data())),
        static_cast<ULWord>(video.rowBytes() * video.format().height));

    if (audioSystem_ != NTV2_AUDIOSYSTEM_INVALID && audio.valid() &&
        audio.interleaved != nullptr) {
      packAudio(audio);
      transfer.SetAudioBuffer(
          reinterpret_cast<ULWord*>(audioScratch_.data()),
          static_cast<ULWord>(audioScratch_.size() * sizeof(int32_t)));
    }

    if (device_.AutoCirculateTransfer(channel_, transfer)) {
      ++framesSent_;
    } else {
      ++framesDropped_;
    }
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("aja"));
    value.set("name", json::Value(name_));
    value.set("running", json::Value(running_));
    value.set("device", json::Value(deviceName_));
    value.set("device_index", json::Value(deviceIndex_));
    value.set("channel", json::Value(static_cast<int>(channel_) + 1));
    value.set("frames", json::Value(framesSent_));
    value.set("frames_dropped", json::Value(framesDropped_));
    value.set("buffer_level", json::Value(bufferLevel_));
    value.set("ring_frames", json::Value(ringFrames_));
    value.set("audio", json::Value(audioSystem_ != NTV2_AUDIOSYSTEM_INVALID));
    return value;
  }

 private:
  /// AJA wants interleaved 32-bit signed samples with exactly the channel count
  /// the audio system was configured for.
  void packAudio(const AudioBlock& audio) {
    const int channels = std::min(audio.channels, audioChannels_);
    audioScratch_.assign(static_cast<size_t>(audio.frames) *
                             static_cast<size_t>(audioChannels_),
                         0);
    for (int f = 0; f < audio.frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        float sample =
            audio.interleaved[static_cast<size_t>(f) * audio.channels + c];
        // Clamp before scaling: a page will produce samples outside [-1, 1] and
        // wrapping turns mild distortion into a full-scale click.
        sample = std::clamp(sample, -1.0f, 1.0f);
        audioScratch_[static_cast<size_t>(f) * audioChannels_ + c] =
            static_cast<int32_t>(sample * 2147483647.0f);
      }
    }
  }

  CNTV2Card device_;
  VideoFormat format_;
  NTV2VideoFormat videoFormat_ = NTV2_FORMAT_UNKNOWN;
  NTV2AudioSystem audioSystem_ = NTV2_AUDIOSYSTEM_INVALID;
  std::string deviceName_;
  int deviceIndex_;
  NTV2Channel channel_;
  int ringFrames_;
  int audioChannels_;
  bool acquired_ = false;
  bool circulating_ = false;

  int64_t framesSent_ = 0;
  int64_t framesDropped_ = 0;
  int bufferLevel_ = 0;
  std::vector<int32_t> audioScratch_;
};

}  // namespace

std::unique_ptr<IOutput> createAjaOutput(const OutputSpec& spec) {
  return std::make_unique<AjaOutput>(spec);
}

}  // namespace weblinked
