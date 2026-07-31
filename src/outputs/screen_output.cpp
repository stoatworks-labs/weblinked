#include "outputs/screen_output.h"

#include "diag/diag.h"

namespace weblinked {

ScreenOutput::ScreenOutput(const OutputSpec& spec)
    : IOutput(spec.name.empty() ? "screen" : spec.name),
      display_(spec.optionInt("display", spec.deviceIndex)),
      scaling_(screenScalingFromString(spec.optionString("scaling", "fit"))),
      window_(createScreenWindow()) {}

ScreenOutput::~ScreenOutput() {
  // Not merely defensive. A source torn down while running destroys its
  // outputs without calling stop(), and a live CVDisplayLink whose callback
  // still holds this object is a use-after-free a few milliseconds later.
  if (running_) {
    ScreenOutput::stop();
  }
}

bool ScreenOutput::start(const VideoFormat& format, std::string& error) {
  if (running_) {
    return true;
  }
  if (!window_->open(format, display_, scaling_, error)) {
    return false;
  }
  submitted_.store(0, std::memory_order_relaxed);
  running_ = true;
  diag::info("screen '%s': %dx%d on display %d, %s, %s", name_.c_str(),
             format.width, format.height, display_,
             screenScalingToString(scaling_), window_->describe().c_str());
  return true;
}

void ScreenOutput::stop() {
  if (!running_) {
    return;
  }
  // Cleared first. close() waits for the display-refresh callback to finish,
  // and a submit() racing in behind it must already be a no-op by then.
  running_ = false;
  window_->close();
}

void ScreenOutput::submit(const VideoFrame& video, const AudioBlock& audio) {
  (void)audio;  // wantsAudio() is false, so the engine sends none.
  if (!running_) {
    return;
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
  window_->present(video);
}

json::Value ScreenOutput::status() const {
  json::Value value = json::Value::object();
  value.set("kind", json::Value("screen"));
  value.set("name", json::Value(name_));
  value.set("running", json::Value(running_));
  value.set("display", json::Value(display_));
  value.set("scaling", json::Value(screenScalingToString(scaling_)));
  value.set("frames", json::Value(submitted_.load(std::memory_order_relaxed)));
  // Presented is paced by the display and submitted by the engine clock, so
  // these two agree only when the head happens to run at the video rate. The
  // gap is the point: it is how an operator tells frame repetition on a 60 Hz
  // monitor fed at 50 from something actually going wrong.
  value.set("presented", json::Value(window_->presentedCount()));
  value.set("dropped", json::Value(window_->droppedCount()));
  value.set("renderer", json::Value(window_->describe()));
  return value;
}

std::unique_ptr<IOutput> createScreenOutput(const OutputSpec& spec) {
  return std::make_unique<ScreenOutput>(spec);
}

}  // namespace weblinked
