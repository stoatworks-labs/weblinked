#include "core/audio_route.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace weblinked {
namespace {

/// dB to linear. -inf is spelled as anything at or below this, because a fader
/// pulled to the bottom should be silence and not -120 dB of dither.
constexpr double kMinusInfinityDb = -100.0;

float linearFromDb(double db) {
  if (db <= kMinusInfinityDb) {
    return 0.0f;
  }
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

}  // namespace

AudioRoute AudioRoute::fromOptions(const json::Value& options,
                                   std::string* error) {
  AudioRoute route;
  const json::Value& audio = options["audio"];
  if (!audio.isObject()) {
    // Absent means identity, which is what every output written before routing
    // existed already does. Only a present-but-wrong value is an error.
    if (!audio.isNull() && error != nullptr) {
      *error = "audio: expected an object";
    }
    return route;
  }

  route.mute = audio["mute"].asBool(false);

  if (audio["gain_db"].isNumber()) {
    route.gain = linearFromDb(audio["gain_db"].asDouble(0.0));
  }

  if (audio["delay_ms"].isNumber()) {
    const int delay = audio["delay_ms"].asInt(0);
    if (delay < 0 || delay > kMaxDelayMs) {
      if (error != nullptr) {
        *error = "audio: delay_ms must be between 0 and " +
                 std::to_string(kMaxDelayMs);
      }
      return AudioRoute{};
    }
    route.delayMs = delay;
  }

  if (audio["channels"].isNumber()) {
    const int channels = audio["channels"].asInt(0);
    if (channels < 0 || channels > kMaxChannels) {
      if (error != nullptr) {
        *error = "audio: channels must be between 0 and " +
                 std::to_string(kMaxChannels);
      }
      return AudioRoute{};
    }
    route.channels = channels;
  }

  const json::Value& map = audio["map"];
  if (map.isArray()) {
    if (map.size() > static_cast<size_t>(kMaxChannels)) {
      if (error != nullptr) {
        *error = "audio: map has more than " + std::to_string(kMaxChannels) +
                 " entries";
      }
      return AudioRoute{};
    }
    route.map.reserve(map.size());
    for (size_t i = 0; i < map.size(); ++i) {
      const json::Value& entry = map.at(i);
      // null is how an operator spells "leave this one silent" in a file; -1 is
      // how the control page sends the same thing from a <select>.
      if (entry.isNull()) {
        route.map.push_back(kSilent);
        continue;
      }
      if (!entry.isNumber()) {
        if (error != nullptr) {
          *error = "audio: map entry " + std::to_string(i) +
                   " is not a channel number";
        }
        return AudioRoute{};
      }
      const int channel = entry.asInt(kSilent);
      if (channel < kSilent || channel >= kMaxChannels) {
        if (error != nullptr) {
          *error = "audio: map entry " + std::to_string(i) + " is out of range";
        }
        return AudioRoute{};
      }
      route.map.push_back(channel);
    }
  } else if (!map.isNull()) {
    if (error != nullptr) {
      *error = "audio: map must be an array";
    }
    return AudioRoute{};
  }

  return route;
}

json::Value AudioRoute::toJson() const {
  // Identity is spelled as absence. Writing out a full block of defaults for
  // every output would double the size of a settings file to say nothing.
  if (mute || gain != 1.0f || delayMs != 0 || channels != 0 || !map.empty()) {
    json::Value value = json::Value::object();
    if (mute) {
      value.set("mute", json::Value(true));
    }
    if (gain != 1.0f) {
      const double db = gain <= 0.0f ? kMinusInfinityDb
                                     : 20.0 * std::log10(static_cast<double>(gain));
      value.set("gain_db", json::Value(db));
    }
    if (delayMs != 0) {
      value.set("delay_ms", json::Value(delayMs));
    }
    if (channels != 0) {
      value.set("channels", json::Value(channels));
    }
    if (!map.empty()) {
      json::Value entries = json::Value::array();
      for (int channel : map) {
        entries.push(json::Value(channel));
      }
      value.set("map", entries);
    }
    return value;
  }
  return json::Value();
}

int AudioRoute::destinationChannels(int sourceChannels) const {
  if (channels > 0) {
    return channels;
  }
  if (!map.empty()) {
    return static_cast<int>(map.size());
  }
  return sourceChannels;
}

bool AudioRoute::isIdentity(int sourceChannels) const {
  if (mute || gain != 1.0f || delayMs != 0) {
    return false;
  }
  if (destinationChannels(sourceChannels) != sourceChannels) {
    return false;
  }
  for (size_t i = 0; i < map.size(); ++i) {
    if (map[i] != static_cast<int>(i)) {
      return false;
    }
  }
  return true;
}

void AudioRouter::configure(const AudioRoute& route) {
  route_ = route;
  // Force prepare() to rebuild: the delay length may have changed, and a line
  // sized for the old one would trail by the wrong amount.
  preparedChannels_ = 0;
  preparedRate_ = 0;
  reset();
}

void AudioRouter::reset() {
  for (auto& line : delayLines_) {
    std::fill(line.begin(), line.end(), 0.0f);
  }
  delayPos_ = 0;
}

void AudioRouter::prepare(int destChannels, int sampleRate) {
  if (destChannels == preparedChannels_ && sampleRate == preparedRate_) {
    return;
  }
  preparedChannels_ = destChannels;
  preparedRate_ = sampleRate;

  delayFrames_ = static_cast<int>(static_cast<int64_t>(route_.delayMs) *
                                  sampleRate / 1000);

  planes_.assign(static_cast<size_t>(destChannels), {});
  planePointers_.assign(static_cast<size_t>(destChannels), nullptr);
  delayLines_.assign(static_cast<size_t>(destChannels),
                     std::vector<float>(static_cast<size_t>(delayFrames_), 0.0f));
  delayPos_ = 0;
}

AudioBlock AudioRouter::apply(const AudioBlock& in) {
  if (!in.valid() || in.planes == nullptr) {
    return in;
  }
  if (route_.isIdentity(in.channels)) {
    return in;
  }

  const int frames = in.frames;
  const int destChannels = route_.destinationChannels(in.channels);
  prepare(destChannels, in.sampleRate);

  for (int c = 0; c < destChannels; ++c) {
    auto& plane = planes_[static_cast<size_t>(c)];
    plane.resize(static_cast<size_t>(frames));
    planePointers_[static_cast<size_t>(c)] = plane.data();

    // Which source channel feeds this one. An empty map is straight-through,
    // and a destination past the end of the source is silence rather than a
    // wrap — a 4-channel leg fed by a stereo page wants 3 and 4 quiet, not a
    // second copy of 1 and 2 that nobody asked for.
    int source = c;
    if (!route_.map.empty()) {
      source = c < static_cast<int>(route_.map.size())
                   ? route_.map[static_cast<size_t>(c)]
                   : AudioRoute::kSilent;
    }
    const bool silent =
        route_.mute || source == AudioRoute::kSilent || source >= in.channels;

    if (silent) {
      std::fill(plane.begin(), plane.end(), 0.0f);
    } else {
      const float* src = in.planes[source];
      const float gain = route_.gain;
      for (int f = 0; f < frames; ++f) {
        plane[static_cast<size_t>(f)] = src[f] * gain;
      }
    }
  }

  if (delayFrames_ > 0) {
    // One shared position across the channels, advanced once per sample-frame,
    // so every channel is delayed by exactly the same amount. Delaying them
    // independently is how a stereo image ends up smeared.
    const size_t length = static_cast<size_t>(delayFrames_);
    size_t pos = delayPos_;
    for (int f = 0; f < frames; ++f) {
      for (int c = 0; c < destChannels; ++c) {
        auto& line = delayLines_[static_cast<size_t>(c)];
        auto& plane = planes_[static_cast<size_t>(c)];
        const float incoming = plane[static_cast<size_t>(f)];
        plane[static_cast<size_t>(f)] = line[pos];
        line[pos] = incoming;
      }
      pos = pos + 1 == length ? 0 : pos + 1;
    }
    delayPos_ = pos;
  }

  interleaved_.resize(static_cast<size_t>(frames) *
                      static_cast<size_t>(destChannels));
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < destChannels; ++c) {
      interleaved_[static_cast<size_t>(f) * destChannels + c] =
          planes_[static_cast<size_t>(c)][static_cast<size_t>(f)];
    }
  }

  AudioBlock out;
  out.planes = planePointers_.data();
  out.interleaved = interleaved_.data();
  out.frames = frames;
  out.channels = destChannels;
  out.sampleRate = in.sampleRate;
  return out;
}

}  // namespace weblinked
