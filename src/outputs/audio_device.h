#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/audio_block.h"
#include "core/json.h"

namespace weblinked {

/// A playback device as the control page should list it.
struct AudioDeviceInfo {
  /// What a settings file stores. The device's name, because that is the only
  /// identifier a human can read back off a machine — miniaudio's own
  /// `ma_device_id` is an opaque union that means nothing across a reboot on
  /// some backends and nothing across machines on any of them.
  std::string id;
  std::string name;
  int channels = 0;      ///< as reported by the driver; 0 when it will not say
  bool isDefault = false;
  bool inUse = false;    ///< already open in this process

  json::Value toJson() const;
};

/// One output's claim on a contiguous slice of one device's channels.
///
/// Handed out by the hub and released by destroying it. While it exists the
/// underlying device stays open, so an output that stops and restarts inside a
/// format change does not close and reopen the card underneath the other
/// outputs sharing it.
class AudioDeviceClient {
 public:
  virtual ~AudioDeviceClient() = default;

  /// Producer side, called from the clock thread once per tick. Non-blocking:
  /// audio that will not fit is dropped and counted, never waited on.
  virtual void write(const AudioBlock& audio) = 0;

  /// Drops anything buffered and re-primes. For a restart, so the card does not
  /// open by playing out audio from before the gap.
  virtual void reset() = 0;

  virtual const std::string& deviceName() const = 0;
  virtual int firstChannel() const = 0;
  virtual int channels() const = 0;
  /// Channels the device was actually opened with, which is what a route's
  /// first_channel is bounds-checked against.
  virtual int deviceChannels() const = 0;

  /// Live counters — buffer level, underruns, overruns, resyncs. Called from
  /// the HTTP thread.
  virtual json::Value status() const = 0;
};

/// The one thing in this process that knows several sources exist.
///
/// Everywhere else, sources are deliberately ignorant of each other: that
/// independence is what stops one hung page or one missing card taking the rest
/// down. A sound card is where that has to stop being true. Two sources feeding
/// channels 1-2 and 3-4 of the same interface is the whole point of routable
/// audio outputs, and an operating system will not let a process open one
/// device twice to do it — WASAPI in exclusive mode and ALSA `hw:` will not let
/// anything on the machine open it twice.
///
/// So the hub opens each device exactly once, at its native channel count, and
/// hands out slices. Clients whose slices overlap are summed, which is what an
/// operator asking two sources for the same pair is asking for. This is a
/// contained exception to the rule, at the level the rule was always going to
/// break: the hardware. It buys no other coupling — no source can see another's
/// audio, ask about it, or interfere with it beyond the channels it was
/// explicitly pointed at.
class AudioDeviceHub {
 public:
  static AudioDeviceHub& instance();

  /// Every playback device the system will admit to. Returns false and sets
  /// `error` when the audio subsystem itself will not start, which is different
  /// from a machine that genuinely has no outputs.
  bool devices(std::vector<AudioDeviceInfo>& out, std::string& error);

  /// Opens `deviceId` if it is not already open, and claims `channels`
  /// channels starting at `firstChannel` (0-based).
  ///
  /// `deviceId` is a device name, or empty for the system default. Returns
  /// nullptr with `error` set for a device that has gone away, a slice that
  /// runs off the end of it, or a device that will not open at 48 kHz.
  std::shared_ptr<AudioDeviceClient> open(const std::string& deviceId,
                                          int channels, int firstChannel,
                                          int sampleRate, int bufferMs,
                                          std::string& error);

  /// Closes every device and invalidates every client. For shutdown only:
  /// miniaudio's context has to be torn down before the process exits, and a
  /// device left running past main() faults inside the driver.
  void shutdown();

  AudioDeviceHub(const AudioDeviceHub&) = delete;
  AudioDeviceHub& operator=(const AudioDeviceHub&) = delete;

 private:
  AudioDeviceHub();
  ~AudioDeviceHub();

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace weblinked
