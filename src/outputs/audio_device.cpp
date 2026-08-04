#include "outputs/audio_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/audio_ring.h"
#include "diag/diag.h"
#include "miniaudio.h"

namespace weblinked {
namespace {

/// How many outputs may share one device. Sixteen is well past what an
/// operator will build by hand and keeps the callback's loop over slots a
/// fixed, allocation-free walk.
constexpr int kMaxSlots = 16;

/// The callback is handed whatever the driver feels like asking for, which on
/// some Windows machines is a much larger block than the configured period.
/// Rendering in fixed chunks means the per-device scratch buffer is sized once
/// at open and never grown from inside the callback.
constexpr ma_uint32 kChunkFrames = 512;

/// Every destination downstream runs at 48 kHz and so does the browser, so this
/// is what devices are opened at. miniaudio resamples internally when the card
/// itself will not run at 48 — which is its problem to solve once, rather than
/// ours to solve per output.
constexpr int kDeviceSampleRate = 48000;

json::Value ringStats(const AudioRing& ring) {
  const AudioRing::Stats stats = ring.stats();
  json::Value value = json::Value::object();
  value.set("buffered_frames", json::Value(ring.bufferedFrames()));
  value.set("target_frames", json::Value(ring.targetFrames()));
  value.set("priming", json::Value(ring.priming()));
  value.set("underruns", json::Value(stats.underruns));
  value.set("overruns", json::Value(stats.overruns));
  value.set("resyncs", json::Value(stats.resyncs));
  return value;
}

/// One output's claim, and the ring behind it.
///
/// Slots live and die with the device, never with the client that occupies
/// them. That is deliberate: a slot's ring is read by the driver's callback,
/// and freeing one while a callback is inside it is the kind of fault that
/// lands in the driver's stack with none of our frames on it. Releasing a
/// client clears `active` and leaves the memory alone.
struct Slot {
  std::atomic<bool> active{false};
  bool claimed = false;  ///< guarded by Device::claimMutex_
  AudioRing ring;
  int firstChannel = 0;
  int channels = 0;
};

class Device {
 public:
  Device() = default;

  ~Device() {
    if (initialised_) {
      ma_device_uninit(&device_);
    }
  }

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  bool open(ma_context* context, const ma_device_id* id, const std::string& name,
            std::string& error) {
    name_ = name;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = id;
    config.playback.format = ma_format_f32;
    // Zero means "however many the device has". Asking for a fixed two would
    // make an eight-output interface look like a stereo one, which is exactly
    // the hardware this feature exists for.
    config.playback.channels = 0;
    config.sampleRate = kDeviceSampleRate;
    config.periodSizeInMilliseconds = 10;
    config.dataCallback = &Device::dataCallback;
    config.pUserData = this;

    if (ma_device_init(context, &config, &device_) != MA_SUCCESS) {
      error = "audio device '" + name + "' would not open";
      return false;
    }
    initialised_ = true;
    channels_ = static_cast<int>(device_.playback.channels);
    if (channels_ <= 0) {
      error = "audio device '" + name + "' reports no output channels";
      return false;
    }

    scratch_.assign(static_cast<size_t>(kChunkFrames) *
                        static_cast<size_t>(channels_),
                    0.0f);

    if (ma_device_start(&device_) != MA_SUCCESS) {
      error = "audio device '" + name + "' would not start";
      return false;
    }
    diag::info("audio: opened '%s' — %d channels at %d Hz", name.c_str(),
               channels_, static_cast<int>(device_.sampleRate));
    return true;
  }

  int channels() const { return channels_; }
  const std::string& name() const { return name_; }

  /// Claims a slice. Returns the slot index, or -1 with `error` set.
  int claim(int firstChannel, int channels, int bufferMs, std::string& error) {
    if (firstChannel < 0 || channels <= 0) {
      error = "audio: a route needs at least one channel";
      return -1;
    }
    if (firstChannel + channels > channels_) {
      error = "audio device '" + name_ + "' has " + std::to_string(channels_) +
              " channels; asked for " + std::to_string(channels) +
              " starting at " + std::to_string(firstChannel + 1);
      return -1;
    }

    std::lock_guard<std::mutex> lock(claimMutex_);
    for (int i = 0; i < kMaxSlots; ++i) {
      Slot& slot = slots_[static_cast<size_t>(i)];
      if (slot.claimed) {
        continue;
      }
      // The previous occupant may have left with a callback still walking this
      // slot. Wait for the driver to come round again before touching the ring
      // it might be reading.
      waitForCallbackTurnover();

      slot.ring.configure(channels, kDeviceSampleRate, bufferMs * 2, bufferMs);
      slot.firstChannel = firstChannel;
      slot.channels = channels;
      slot.claimed = true;
      slot.active.store(true, std::memory_order_release);
      return i;
    }
    error = "audio device '" + name_ + "' already has " +
            std::to_string(kMaxSlots) + " outputs on it";
    return -1;
  }

  void release(int index) {
    if (index < 0 || index >= kMaxSlots) {
      return;
    }
    std::lock_guard<std::mutex> lock(claimMutex_);
    Slot& slot = slots_[static_cast<size_t>(index)];
    slot.active.store(false, std::memory_order_release);
    slot.claimed = false;
  }

  Slot& slot(int index) { return slots_[static_cast<size_t>(index)]; }

 private:
  static void dataCallback(ma_device* device, void* output, const void* input,
                           ma_uint32 frames) {
    (void)input;
    auto* self = static_cast<Device*>(device->pUserData);
    if (self != nullptr) {
      self->render(static_cast<float*>(output), frames);
    }
  }

  void render(float* output, ma_uint32 frames) {
    const size_t channels = static_cast<size_t>(channels_);
    // Silence first, then sum in. An unclaimed channel on a shared interface
    // must be silent, not left holding whatever the driver's buffer had.
    std::memset(output, 0, static_cast<size_t>(frames) * channels * sizeof(float));

    for (int i = 0; i < kMaxSlots; ++i) {
      Slot& slot = slots_[static_cast<size_t>(i)];
      if (!slot.active.load(std::memory_order_acquire)) {
        continue;
      }
      const size_t slotChannels = static_cast<size_t>(slot.channels);
      const size_t first = static_cast<size_t>(slot.firstChannel);

      ma_uint32 done = 0;
      while (done < frames) {
        const ma_uint32 chunk = std::min(kChunkFrames, frames - done);
        slot.ring.read(scratch_.data(), static_cast<int>(chunk));
        float* dst = output + static_cast<size_t>(done) * channels;
        for (ma_uint32 f = 0; f < chunk; ++f) {
          const float* src = scratch_.data() + static_cast<size_t>(f) * slotChannels;
          float* row = dst + static_cast<size_t>(f) * channels;
          for (size_t c = 0; c < slotChannels; ++c) {
            row[first + c] += src[c];
          }
        }
        done += chunk;
      }
    }

    callbacks_.fetch_add(1, std::memory_order_release);
  }

  /// Blocks until the driver has completed a callback that began after this
  /// call, so a slot cleared before it is safe to rebuild.
  ///
  /// Bounded, because a device that has stopped — unplugged, or never started —
  /// will never bump the counter and the caller is a control thread that must
  /// not hang on it. Two hundred milliseconds is twenty periods; if nothing has
  /// happened by then, nothing is going to.
  void waitForCallbackTurnover() {
    if (!initialised_ || ma_device_get_state(&device_) != ma_device_state_started) {
      return;
    }
    const uint64_t start = callbacks_.load(std::memory_order_acquire);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (callbacks_.load(std::memory_order_acquire) < start + 2) {
      if (std::chrono::steady_clock::now() > deadline) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ma_device device_{};
  bool initialised_ = false;
  int channels_ = 0;
  std::string name_;
  std::array<Slot, kMaxSlots> slots_;
  std::vector<float> scratch_;
  std::atomic<uint64_t> callbacks_{0};
  std::mutex claimMutex_;
};

class ClientImpl : public AudioDeviceClient {
 public:
  ClientImpl(std::shared_ptr<Device> device, int slot, int firstChannel,
             int channels)
      : device_(std::move(device)),
        slot_(slot),
        firstChannel_(firstChannel),
        channels_(channels) {}

  ~ClientImpl() override {
    if (device_ != nullptr) {
      device_->release(slot_);
    }
  }

  void write(const AudioBlock& audio) override {
    if (!audio.valid() || audio.interleaved == nullptr) {
      return;
    }
    Slot& slot = device_->slot(slot_);

    if (audio.channels == channels_) {
      slot.ring.write(audio.interleaved, audio.frames);
      return;
    }

    // The route should already have produced exactly this many channels; this
    // is the case where an output was reconfigured and the route has not caught
    // up yet. Copy what fits and leave the rest silent rather than writing a
    // block whose stride the ring would read wrong — a stride mismatch turns
    // into a burst of noise at full scale, straight into someone's monitors.
    const int copy = std::min(audio.channels, channels_);
    convert_.assign(static_cast<size_t>(audio.frames) *
                        static_cast<size_t>(channels_),
                    0.0f);
    for (int f = 0; f < audio.frames; ++f) {
      for (int c = 0; c < copy; ++c) {
        convert_[static_cast<size_t>(f) * channels_ + c] =
            audio.interleaved[static_cast<size_t>(f) * audio.channels + c];
      }
    }
    slot.ring.write(convert_.data(), audio.frames);
  }

  void reset() override { device_->slot(slot_).ring.reset(); }

  const std::string& deviceName() const override { return device_->name(); }
  int firstChannel() const override { return firstChannel_; }
  int channels() const override { return channels_; }
  int deviceChannels() const override { return device_->channels(); }

  json::Value status() const override {
    json::Value value = ringStats(device_->slot(slot_).ring);
    value.set("device", json::Value(device_->name()));
    value.set("device_channels", json::Value(device_->channels()));
    value.set("first_channel", json::Value(firstChannel_));
    value.set("channels", json::Value(channels_));
    return value;
  }

 private:
  std::shared_ptr<Device> device_;
  int slot_ = -1;
  int firstChannel_ = 0;
  int channels_ = 0;
  std::vector<float> convert_;
};

}  // namespace

class AudioDeviceHub::Impl {
 public:
  ~Impl() { teardown(); }

  bool ensureContext(std::string& error) {
    if (contextReady_) {
      return true;
    }
    if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
      error = "no audio subsystem available on this machine";
      return false;
    }
    contextReady_ = true;
    return true;
  }

  void teardown() {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
    if (contextReady_) {
      ma_context_uninit(&context_);
      contextReady_ = false;
    }
  }

  std::mutex mutex_;
  ma_context context_{};
  bool contextReady_ = false;
  /// Weak, so a device closes as soon as its last output goes. Holding these
  /// strongly would keep an interface claimed for the life of the process,
  /// which on WASAPI exclusive or ALSA hw: means nothing else on the machine
  /// can have it back.
  std::map<std::string, std::weak_ptr<Device>> devices_;
};

AudioDeviceHub::AudioDeviceHub() : impl_(std::make_unique<Impl>()) {}
AudioDeviceHub::~AudioDeviceHub() = default;

AudioDeviceHub& AudioDeviceHub::instance() {
  static AudioDeviceHub hub;
  return hub;
}

json::Value AudioDeviceInfo::toJson() const {
  json::Value value = json::Value::object();
  value.set("id", json::Value(id));
  value.set("name", json::Value(name));
  value.set("channels", json::Value(channels));
  value.set("default", json::Value(isDefault));
  value.set("in_use", json::Value(inUse));
  return value;
}

bool AudioDeviceHub::devices(std::vector<AudioDeviceInfo>& out,
                             std::string& error) {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (!impl_->ensureContext(error)) {
    return false;
  }

  ma_device_info* playback = nullptr;
  ma_uint32 playbackCount = 0;
  if (ma_context_get_devices(&impl_->context_, &playback, &playbackCount, nullptr,
                             nullptr) != MA_SUCCESS) {
    error = "could not enumerate audio devices";
    return false;
  }

  out.clear();
  out.reserve(playbackCount);
  for (ma_uint32 i = 0; i < playbackCount; ++i) {
    AudioDeviceInfo info;
    info.name = playback[i].name;
    info.id = info.name;
    info.isDefault = playback[i].isDefault != 0;

    // The detailed info — channel counts and rates — is a second, more
    // expensive call per device, and some backends only fill it in there.
    ma_device_info detail;
    if (ma_context_get_device_info(&impl_->context_, ma_device_type_playback,
                                   &playback[i].id, &detail) == MA_SUCCESS) {
      ma_uint32 channels = 0;
      for (ma_uint32 f = 0; f < detail.nativeDataFormatCount; ++f) {
        channels = std::max(channels, detail.nativeDataFormats[f].channels);
      }
      info.channels = static_cast<int>(channels);
    }

    const auto it = impl_->devices_.find(info.id);
    info.inUse = it != impl_->devices_.end() && !it->second.expired();
    out.push_back(std::move(info));
  }
  return true;
}

std::shared_ptr<AudioDeviceClient> AudioDeviceHub::open(
    const std::string& deviceId, int channels, int firstChannel, int sampleRate,
    int bufferMs, std::string& error) {
  if (sampleRate != 0 && sampleRate != kDeviceSampleRate) {
    // Not a limitation worth hiding: the browser is pinned to 48 kHz in
    // RenderClient::GetAudioParameters, so a device asked to run at anything
    // else would be resampling audio that was never at that rate to begin with.
    error = "audio devices run at 48 kHz; asked for " + std::to_string(sampleRate);
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (!impl_->ensureContext(error)) {
    return nullptr;
  }

  std::shared_ptr<Device> device;
  const auto existing = impl_->devices_.find(deviceId);
  if (existing != impl_->devices_.end()) {
    device = existing->second.lock();
  }

  if (device == nullptr) {
    // Resolve the name to an id every time rather than caching one: devices are
    // unplugged between shows, and a stale id opens the wrong card silently.
    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&impl_->context_, &playback, &playbackCount,
                               nullptr, nullptr) != MA_SUCCESS) {
      error = "could not enumerate audio devices";
      return nullptr;
    }

    const ma_device_id* id = nullptr;
    std::string name = deviceId;
    if (!deviceId.empty()) {
      bool found = false;
      for (ma_uint32 i = 0; i < playbackCount; ++i) {
        if (deviceId == playback[i].name) {
          id = &playback[i].id;
          found = true;
          break;
        }
      }
      if (!found) {
        error = "no audio device called '" + deviceId + "'";
        return nullptr;
      }
    } else {
      for (ma_uint32 i = 0; i < playbackCount; ++i) {
        if (playback[i].isDefault != 0) {
          name = playback[i].name;
          break;
        }
      }
      if (name.empty()) {
        name = "system default";
      }
    }

    device = std::make_shared<Device>();
    if (!device->open(&impl_->context_, id, name, error)) {
      return nullptr;
    }
    impl_->devices_[deviceId] = device;
  }

  const int slot = device->claim(firstChannel, channels, bufferMs, error);
  if (slot < 0) {
    return nullptr;
  }
  return std::make_shared<ClientImpl>(device, slot, firstChannel, channels);
}

void AudioDeviceHub::shutdown() { impl_->teardown(); }

}  // namespace weblinked
