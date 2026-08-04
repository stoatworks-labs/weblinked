#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace weblinked {

/// A lock-free interleaved float ring between the frame clock and a sound
/// card's callback.
///
/// AudioFifo already reconciles the browser's packet size with the frame rate,
/// and does it with a mutex — justified there because both ends are ordinary
/// threads. This one cannot: the consumer is a real-time audio callback, and a
/// callback that blocks on a mutex held by a descheduled clock thread is a
/// dropout on every device sharing that thread, not just ours.
///
/// Strictly one producer and one consumer, and they own one index each. That
/// single-ownership is what makes it correct without a lock, so the rules are
/// worth stating plainly:
///
///   * the producer only ever advances `write_`,
///   * the consumer only ever advances `read_`.
///
/// Which is why an overflow is *not* handled by having the producer discard the
/// oldest audio — that would mean writing the consumer's index from the wrong
/// thread. The producer drops what will not fit and says so; the consumer is
/// the one that re-centres a ring that has drifted long, because it owns the
/// index that does it.
///
/// The two ends also run on unrelated clocks. The card's crystal and the video
/// frame clock are never the same oscillator, so over a long enough show the
/// buffer level walks in one direction whatever it started at. That is not a
/// bug to be asserted away — it is measured, corrected in one jump when it
/// exceeds a window, and counted so the control page can show it happening.
class AudioRing {
 public:
  /// Sizes the ring for `channels` at `sampleRate`, holding `capacityMs` of
  /// audio and aiming to sit `targetMs` full. Discards anything buffered.
  void configure(int channels, int sampleRate, int capacityMs, int targetMs);

  int channels() const { return channels_; }
  int sampleRate() const { return sampleRate_; }
  int targetFrames() const { return targetFrames_; }

  /// Producer side. Appends `frames` sample-frames of interleaved float.
  /// Returns how many were taken; anything short is counted as an overrun and
  /// means the card is not consuming as fast as the clock is producing.
  int write(const float* interleaved, int frames);

  /// Consumer side, called from the audio callback. Writes exactly `frames`
  /// sample-frames into `dst`, padding with silence when there are not enough
  /// and counting that as an underrun.
  ///
  /// Also re-centres: a ring that has drifted past `capacity`-ish long is
  /// dropped back to the target in one step rather than a sample at a time,
  /// because a slow drip of dropped samples is a stream of small artefacts
  /// where one jump is a single audible one every few hours.
  int read(float* dst, int frames);

  /// True until the ring has filled to its target for the first time, and again
  /// after an underrun. The consumer emits silence while it is set rather than
  /// playing out a nearly-empty ring, which would underrun again immediately
  /// and keep doing so — one clean gap beats a hundred short ones.
  bool priming() const { return priming_.load(std::memory_order_relaxed); }

  int bufferedFrames() const;

  struct Stats {
    int64_t framesWritten = 0;
    int64_t framesRead = 0;
    int64_t underruns = 0;
    int64_t overruns = 0;
    int64_t resyncs = 0;
  };
  Stats stats() const;

  void reset();

 private:
  int channels_ = 0;
  int sampleRate_ = 0;
  int capacityFrames_ = 0;
  int targetFrames_ = 0;

  std::vector<float> buffer_;  ///< capacityFrames_ * channels_ floats

  /// Free-running frame counters, wrapped only on indexing. Their difference is
  /// the fill level, which stays correct across a wrap in a way that two
  /// wrapped positions plus a "full" flag never quite does.
  std::atomic<uint64_t> write_{0};
  std::atomic<uint64_t> read_{0};
  std::atomic<bool> priming_{true};

  std::atomic<int64_t> framesWritten_{0};
  std::atomic<int64_t> framesRead_{0};
  std::atomic<int64_t> underruns_{0};
  std::atomic<int64_t> overruns_{0};
  std::atomic<int64_t> resyncs_{0};
};

}  // namespace weblinked
