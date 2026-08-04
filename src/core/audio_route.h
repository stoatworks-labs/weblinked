#pragma once

#include <string>
#include <vector>

#include "core/audio_block.h"
#include "core/json.h"

namespace weblinked {

/// What happens to a source's audio on its way to one particular output.
///
/// Every output carries one of these, not just the ones that drive a sound
/// card: an operator who sends a page to SDI and to an analogue pair wants the
/// SDI leg at unity and the analogue leg trimmed, and wants the second pair of
/// a four-channel card fed from the same stereo page. Doing that at the output
/// rather than at the source is what keeps the source a single stereo stream
/// that every leg can treat differently.
///
/// Pure data, parsed and validated without linking CEF or any vendor SDK, so
/// the whole of it is unit-testable.
struct AudioRoute {
  /// Silence rather than absence. A muted leg still emits the tick's exact
  /// sample count: a receiver that suddenly stops being sent audio treats it as
  /// a dead stream and some of them resync noisily when it comes back.
  bool mute = false;

  /// Linear, parsed from `gain_db`. Kept linear here because it is applied per
  /// sample and a dB-to-linear conversion per sample is pure waste.
  float gain = 1.0f;

  /// Milliseconds of delay applied to this leg. Positive only: audio can be
  /// pushed later to line up with a video path that buffers, but it cannot be
  /// pulled earlier than samples that have not been produced yet. Trimming the
  /// other direction means delaying the *other* leg.
  int delayMs = 0;

  /// How many channels this leg emits. 0 means "as many as the source has",
  /// which is what every existing output has always assumed.
  int channels = 0;

  /// map[destinationChannel] = source channel, or kSilent. Empty means
  /// straight-through: destination c takes source c, silence past the end.
  std::vector<int> map;

  static constexpr int kSilent = -1;
  /// A page could in principle open a 32-channel context; a map longer than
  /// this is a config-file typo, not an intention.
  static constexpr int kMaxChannels = 64;
  /// Two seconds. Enough for any sane lip-sync trim, and it bounds what a
  /// mistyped `delay_ms` can allocate.
  static constexpr int kMaxDelayMs = 2000;

  /// Reads the `audio` object out of an output's options. A missing object is
  /// not an error — it is the identity route, which is what every output
  /// configured before this existed means.
  static AudioRoute fromOptions(const json::Value& options,
                                std::string* error = nullptr);

  /// The `audio` object alone. Returns null when this is the identity route, so
  /// a saved settings file does not grow a block of defaults per output.
  json::Value toJson() const;

  /// How many channels this route emits for a source with `sourceChannels`.
  int destinationChannels(int sourceChannels) const;

  /// True when applying this route to a `sourceChannels`-wide block would copy
  /// it unchanged — which lets the engine hand the shared block straight to the
  /// output with no per-output copy at all.
  bool isIdentity(int sourceChannels) const;
};

/// Applies an AudioRoute, owning the buffers the result points into.
///
/// One router per output, lived on the clock thread. `apply` returns a block
/// whose pointers are valid until the next call on the same router — the same
/// contract the engine's own per-tick audio buffers already have.
class AudioRouter {
 public:
  void configure(const AudioRoute& route);
  const AudioRoute& route() const { return route_; }

  /// Returns `in` itself for an identity route, and a routed copy otherwise.
  /// An invalid `in` is returned unchanged: there is nothing to gain or map.
  AudioBlock apply(const AudioBlock& in);

  /// Drops any delayed audio. Called when an output restarts, so a leg with a
  /// delay does not open by playing out samples from before the gap.
  void reset();

 private:
  /// (Re)sizes the delay lines. Cheap and idempotent; the sample rate is not
  /// known until audio actually arrives, which is why this is not in configure.
  void prepare(int destChannels, int sampleRate);

  AudioRoute route_;
  int preparedChannels_ = 0;
  int preparedRate_ = 0;
  int delayFrames_ = 0;

  std::vector<std::vector<float>> planes_;
  std::vector<const float*> planePointers_;
  std::vector<float> interleaved_;

  /// One ring per destination channel, exactly delayFrames_ long. Read and
  /// written at the same position on the same pass, so the sample taken out is
  /// the one put in a whole line ago — which is why the length *is* the delay
  /// and an extra guard element would make it delayFrames_ + 1.
  std::vector<std::vector<float>> delayLines_;
  size_t delayPos_ = 0;
};

}  // namespace weblinked
