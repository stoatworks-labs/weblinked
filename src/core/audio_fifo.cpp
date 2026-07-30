#include "core/audio_fifo.h"

#include <algorithm>
#include <cstring>

namespace weblinked {

AudioFifo::AudioFifo() = default;

void AudioFifo::configure(int channels, int sampleRate, int capacityFrames) {
  std::lock_guard<std::mutex> lock(mutex_);
  channels_ = std::max(0, channels);
  sampleRate_ = std::max(0, sampleRate);
  capacity_ = std::max(1, capacityFrames);
  planes_.assign(static_cast<size_t>(channels_),
                 std::vector<float>(static_cast<size_t>(capacity_), 0.0f));
  writePos_ = 0;
  readPos_ = 0;
  filled_ = 0;
  stats_ = Stats{};
}

int AudioFifo::channels() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return channels_;
}

int AudioFifo::sampleRate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sampleRate_;
}

void AudioFifo::write(const float* const* planes, int frames) {
  if (frames <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (channels_ == 0 || capacity_ == 0) {
    return;
  }

  // A packet larger than the whole buffer can only contribute its tail.
  int offset = 0;
  bool overran = false;
  if (frames > capacity_) {
    offset = frames - capacity_;
    frames = capacity_;
    overran = true;
  }
  const int room = capacity_ - filled_;
  if (frames > room) {
    overran = true;
  }

  for (int c = 0; c < channels_; ++c) {
    const float* src = planes[c] + offset;
    auto& plane = planes_[static_cast<size_t>(c)];
    size_t pos = writePos_;
    int remaining = frames;
    while (remaining > 0) {
      const int chunk =
          std::min<int>(remaining, static_cast<int>(capacity_ - pos));
      std::memcpy(plane.data() + pos, src, static_cast<size_t>(chunk) * sizeof(float));
      src += chunk;
      pos = (pos + static_cast<size_t>(chunk)) % static_cast<size_t>(capacity_);
      remaining -= chunk;
    }
  }

  writePos_ = (writePos_ + static_cast<size_t>(frames)) % static_cast<size_t>(capacity_);
  stats_.framesWritten += frames;

  if (frames > room) {
    // Overwrote unread audio: advance the read cursor to the oldest surviving
    // sample so the consumer never reads a torn mixture of old and new.
    const int lost = frames - room;
    readPos_ = (readPos_ + static_cast<size_t>(lost)) % static_cast<size_t>(capacity_);
    filled_ = capacity_;
  } else {
    filled_ += frames;
  }

  if (overran) {
    ++stats_.overruns;
  }
}

int AudioFifo::availableFramesLocked() const { return filled_; }

int AudioFifo::availableFrames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return availableFramesLocked();
}

int AudioFifo::readInterleaved(float* dst, int frames) {
  if (frames <= 0 || dst == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (channels_ == 0) {
    return 0;
  }

  const int have = std::min(frames, filled_);
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < channels_; ++c) {
      float sample = 0.0f;
      if (f < have) {
        const size_t pos =
            (readPos_ + static_cast<size_t>(f)) % static_cast<size_t>(capacity_);
        sample = planes_[static_cast<size_t>(c)][pos];
      }
      dst[static_cast<size_t>(f) * channels_ + c] = sample;
    }
  }

  readPos_ = (readPos_ + static_cast<size_t>(have)) % static_cast<size_t>(capacity_);
  filled_ -= have;
  stats_.framesRead += have;
  if (have < frames) {
    ++stats_.underruns;
  }
  return have;
}

int AudioFifo::readPlanar(float* const* planes, int frames) {
  if (frames <= 0 || planes == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (channels_ == 0) {
    return 0;
  }

  const int have = std::min(frames, filled_);
  for (int c = 0; c < channels_; ++c) {
    float* dst = planes[c];
    const auto& plane = planes_[static_cast<size_t>(c)];
    size_t pos = readPos_;
    int remaining = have;
    while (remaining > 0) {
      const int chunk =
          std::min<int>(remaining, static_cast<int>(capacity_ - pos));
      std::memcpy(dst, plane.data() + pos, static_cast<size_t>(chunk) * sizeof(float));
      dst += chunk;
      pos = (pos + static_cast<size_t>(chunk)) % static_cast<size_t>(capacity_);
      remaining -= chunk;
    }
    if (have < frames) {
      std::memset(dst, 0, static_cast<size_t>(frames - have) * sizeof(float));
    }
  }

  readPos_ = (readPos_ + static_cast<size_t>(have)) % static_cast<size_t>(capacity_);
  filled_ -= have;
  stats_.framesRead += have;
  if (have < frames) {
    ++stats_.underruns;
  }
  return have;
}

AudioFifo::Stats AudioFifo::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void AudioFifo::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  writePos_ = 0;
  readPos_ = 0;
  filled_ = 0;
  for (auto& plane : planes_) {
    std::fill(plane.begin(), plane.end(), 0.0f);
  }
}

}  // namespace weblinked
