#include "core/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace weblinked {

void AudioRing::configure(int channels, int sampleRate, int capacityMs,
                          int targetMs) {
  channels_ = std::max(channels, 0);
  sampleRate_ = std::max(sampleRate, 0);
  capacityFrames_ =
      static_cast<int>(static_cast<int64_t>(sampleRate_) * capacityMs / 1000);
  targetFrames_ =
      static_cast<int>(static_cast<int64_t>(sampleRate_) * targetMs / 1000);
  // The target has to leave room on both sides: a ring that aims to sit full
  // overruns on the first early write, and one that aims to sit empty
  // underruns on the first late one.
  targetFrames_ = std::clamp(targetFrames_, 0, std::max(capacityFrames_ / 2, 0));

  buffer_.assign(static_cast<size_t>(std::max(capacityFrames_, 0)) *
                     static_cast<size_t>(channels_),
                 0.0f);
  reset();
}

void AudioRing::reset() {
  write_.store(0, std::memory_order_relaxed);
  read_.store(0, std::memory_order_relaxed);
  priming_.store(true, std::memory_order_relaxed);
  std::fill(buffer_.begin(), buffer_.end(), 0.0f);
}

int AudioRing::bufferedFrames() const {
  const uint64_t write = write_.load(std::memory_order_acquire);
  const uint64_t read = read_.load(std::memory_order_acquire);
  return static_cast<int>(write - read);
}

int AudioRing::write(const float* interleaved, int frames) {
  if (interleaved == nullptr || frames <= 0 || channels_ <= 0 ||
      capacityFrames_ <= 0) {
    return 0;
  }

  const uint64_t write = write_.load(std::memory_order_relaxed);
  const uint64_t read = read_.load(std::memory_order_acquire);
  const int buffered = static_cast<int>(write - read);
  const int space = capacityFrames_ - buffered;
  const int taken = std::min(frames, std::max(space, 0));
  if (taken < frames) {
    overruns_.fetch_add(1, std::memory_order_relaxed);
  }
  if (taken <= 0) {
    return 0;
  }

  // At most two memcpys: up to the end of the buffer, then from the start.
  const size_t offset = static_cast<size_t>(write % static_cast<uint64_t>(capacityFrames_));
  const size_t firstFrames =
      std::min(static_cast<size_t>(taken), static_cast<size_t>(capacityFrames_) - offset);
  const size_t channels = static_cast<size_t>(channels_);
  std::memcpy(buffer_.data() + offset * channels, interleaved,
              firstFrames * channels * sizeof(float));
  if (firstFrames < static_cast<size_t>(taken)) {
    std::memcpy(buffer_.data(), interleaved + firstFrames * channels,
                (static_cast<size_t>(taken) - firstFrames) * channels * sizeof(float));
  }

  write_.store(write + static_cast<uint64_t>(taken), std::memory_order_release);
  framesWritten_.fetch_add(taken, std::memory_order_relaxed);

  if (priming_.load(std::memory_order_relaxed) &&
      buffered + taken >= targetFrames_) {
    priming_.store(false, std::memory_order_release);
  }
  return taken;
}

int AudioRing::read(float* dst, int frames) {
  if (dst == nullptr || frames <= 0) {
    return 0;
  }
  const size_t channels = static_cast<size_t>(std::max(channels_, 0));
  if (channels == 0 || capacityFrames_ <= 0) {
    return 0;
  }

  if (priming_.load(std::memory_order_acquire)) {
    std::memset(dst, 0, static_cast<size_t>(frames) * channels * sizeof(float));
    return 0;
  }

  uint64_t read = read_.load(std::memory_order_relaxed);
  const uint64_t write = write_.load(std::memory_order_acquire);
  int buffered = static_cast<int>(write - read);

  // Drift correction. The card's clock and the frame clock are independent, so
  // over hours the level walks one way; when it has walked far enough that the
  // added latency is audible against the video, throw the excess away in one
  // go. Twice the target is the threshold because that is the point at which
  // the extra delay exceeds the delay the operator asked for.
  if (targetFrames_ > 0 && buffered > targetFrames_ * 2) {
    const int excess = buffered - targetFrames_;
    read += static_cast<uint64_t>(excess);
    buffered -= excess;
    resyncs_.fetch_add(1, std::memory_order_relaxed);
  }

  const int taken = std::min(frames, std::max(buffered, 0));
  if (taken > 0) {
    const size_t offset =
        static_cast<size_t>(read % static_cast<uint64_t>(capacityFrames_));
    const size_t firstFrames =
        std::min(static_cast<size_t>(taken), static_cast<size_t>(capacityFrames_) - offset);
    std::memcpy(dst, buffer_.data() + offset * channels,
                firstFrames * channels * sizeof(float));
    if (firstFrames < static_cast<size_t>(taken)) {
      std::memcpy(dst + firstFrames * channels, buffer_.data(),
                  (static_cast<size_t>(taken) - firstFrames) * channels * sizeof(float));
    }
    read += static_cast<uint64_t>(taken);
    framesRead_.fetch_add(taken, std::memory_order_relaxed);
  }

  if (taken < frames) {
    std::memset(dst + static_cast<size_t>(taken) * channels, 0,
                static_cast<size_t>(frames - taken) * channels * sizeof(float));
    underruns_.fetch_add(1, std::memory_order_relaxed);
    // Go back to priming rather than limping along a nearly-empty ring, which
    // would underrun again on the very next callback and keep doing so.
    priming_.store(true, std::memory_order_release);
  }

  read_.store(read, std::memory_order_release);
  return taken;
}

AudioRing::Stats AudioRing::stats() const {
  Stats out;
  out.framesWritten = framesWritten_.load(std::memory_order_relaxed);
  out.framesRead = framesRead_.load(std::memory_order_relaxed);
  out.underruns = underruns_.load(std::memory_order_relaxed);
  out.overruns = overruns_.load(std::memory_order_relaxed);
  out.resyncs = resyncs_.load(std::memory_order_relaxed);
  return out;
}

}  // namespace weblinked
