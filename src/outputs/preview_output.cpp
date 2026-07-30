#include "outputs/preview_output.h"

#include <algorithm>

#include "core/pixel_convert.h"

namespace weblinked {

PreviewOutput::PreviewOutput(const OutputSpec& spec)
    : IOutput(spec.name.empty() ? "preview" : spec.name),
      factor_(std::clamp(spec.optionInt("factor", 4), 1, 16)) {}

bool PreviewOutput::start(const VideoFormat& format, std::string& error) {
  (void)error;
  std::lock_guard<std::mutex> lock(mutex_);
  sourceFormat_ = format;
  width_ = format.width / factor_;
  height_ = format.height / factor_;
  // A zero-sized preview would be pointless but should not be fatal; clamp the
  // factor down instead of failing the whole output.
  while ((width_ < 16 || height_ < 16) && factor_ > 1) {
    --factor_;
    width_ = format.width / factor_;
    height_ = format.height / factor_;
  }
  buffer_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
  fillBlackBgra(buffer_.data(), width_ * 4, width_, height_);
  frames_ = 0;
  running_ = true;
  return true;
}

void PreviewOutput::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  running_ = false;
}

void PreviewOutput::submit(const VideoFrame& video, const AudioBlock& audio) {
  // Audio peaks are cheap here and are the only level indication the control UI
  // has, so they are worth computing even though the preview has no audio path.
  if (audio.valid() && audio.interleaved != nullptr) {
    float peak = 0.0f;
    const int samples = audio.frames * audio.channels;
    for (int i = 0; i < samples; ++i) {
      peak = std::max(peak, std::fabs(audio.interleaved[i]));
    }
    audioPeak_.store(peak, std::memory_order_relaxed);
  } else {
    audioPeak_.store(0.0f, std::memory_order_relaxed);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_ || buffer_.empty()) {
    return;
  }
  if (video.format().width != sourceFormat_.width ||
      video.format().height != sourceFormat_.height) {
    return;  // a resize is in flight; skip rather than scale the wrong shape
  }
  downscaleBgra(video.data(), video.rowBytes(), sourceFormat_.width,
                sourceFormat_.height, buffer_.data(), width_ * 4, factor_);
  ++frames_;
  sequence_ = video.sequence();
}

PreviewOutput::Snapshot PreviewOutput::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot out;
  out.width = width_;
  out.height = height_;
  out.sequence = sequence_;
  out.pixels = buffer_;
  return out;
}

json::Value PreviewOutput::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  json::Value value = json::Value::object();
  value.set("kind", json::Value("preview"));
  value.set("name", json::Value(name_));
  value.set("running", json::Value(running_));
  value.set("width", json::Value(width_));
  value.set("height", json::Value(height_));
  value.set("factor", json::Value(factor_));
  value.set("frames", json::Value(frames_));
  value.set("audio_peak",
            json::Value(static_cast<double>(audioPeak_.load(std::memory_order_relaxed))));
  return value;
}

std::unique_ptr<IOutput> createPreviewOutput(const OutputSpec& spec) {
  return std::make_unique<PreviewOutput>(spec);
}

PreviewOutput* asPreview(IOutput* output) {
  if (output == nullptr || output->kind() != "preview") {
    return nullptr;
  }
  return static_cast<PreviewOutput*>(output);
}

}  // namespace weblinked
