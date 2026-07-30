#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace weblinked {

/// A planar 32-bit float ring buffer between the browser's audio thread and the
/// engine's frame clock.
///
/// The browser delivers audio in its own packet size (typically 10 ms) with no
/// relationship to the video frame rate; outputs want exactly one frame's worth
/// of samples per tick. This is the buffer that reconciles the two.
///
/// One producer (the CEF audio thread), one consumer (the engine clock thread).
/// A mutex rather than a lock-free ring: packets arrive at ~100 Hz, the critical
/// section is a memcpy, and the alternative is a class of bug that only shows up
/// on someone else's machine three hours into a show.
///
/// Underruns are filled with silence and counted, never blocked on — an output
/// that stalls waiting for audio would take video down with it.
class AudioFifo {
 public:
  AudioFifo();

  /// (Re)configures the buffer. Called when the browser's audio stream starts,
  /// which can happen more than once per session and with different parameters.
  /// Discards anything buffered.
  void configure(int channels, int sampleRate, int capacityFrames);

  int channels() const;
  int sampleRate() const;

  /// Appends `frames` sample-frames from `planes[channel][frame]`.
  /// Drops the oldest audio on overflow, because the newest is what an
  /// operator is about to hear.
  void write(const float* const* planes, int frames);

  /// Reads exactly `frames` sample-frames into interleaved `dst`, which must
  /// hold frames * channels floats. Missing samples are written as silence.
  /// Returns how many real sample-frames were available.
  int readInterleaved(float* dst, int frames);

  /// As above, planar: planes[channel] must each hold `frames` floats.
  int readPlanar(float* const* planes, int frames);

  int availableFrames() const;

  struct Stats {
    int64_t framesWritten = 0;
    int64_t framesRead = 0;
    int64_t underruns = 0;   // reads that had to insert silence
    int64_t overruns = 0;    // writes that had to discard old audio
  };
  Stats stats() const;

  void reset();

 private:
  int availableFramesLocked() const;

  mutable std::mutex mutex_;
  int channels_ = 0;
  int sampleRate_ = 0;
  int capacity_ = 0;  // in sample-frames
  size_t writePos_ = 0;
  size_t readPos_ = 0;
  int filled_ = 0;
  /// One contiguous vector per channel; index is the ring position.
  std::vector<std::vector<float>> planes_;
  Stats stats_;
};

}  // namespace weblinked
