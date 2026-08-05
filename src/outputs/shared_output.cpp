#include "outputs/shared_output.h"

#include "diag/diag.h"

namespace weblinked {

SharedOutput::SharedOutput(const OutputSpec& spec)
    : IOutput(spec.name.empty() ? "WebLinked" : spec.name),
      surface_(createSharedSurface()) {}

SharedOutput::~SharedOutput() {
  // A source torn down while running destroys its outputs without calling
  // stop(), and a published surface that outlives this object leaves a dead
  // entry in every consumer's source list until they time it out.
  if (running_) {
    SharedOutput::stop();
  }
}

bool SharedOutput::start(const VideoFormat& format, std::string& error) {
  if (running_) {
    return true;
  }
  if (!surface_->open(format, name_, error)) {
    return false;
  }
  submitted_.store(0, std::memory_order_relaxed);
  running_ = true;
  diag::info("%s '%s': %dx%d, %s", sharedSurfaceProtocol(), name_.c_str(),
             format.width, format.height, surface_->describe().c_str());
  return true;
}

void SharedOutput::stop() {
  if (!running_) {
    return;
  }
  // Cleared first, so a submit() racing in behind close() is already a no-op.
  running_ = false;
  surface_->close();
}

void SharedOutput::submit(const VideoFrame& video, const AudioBlock& audio) {
  (void)audio;  // wantsAudio() is false, so the engine sends none.
  if (!running_) {
    return;
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
  // The skip-when-nobody-is-listening test lives inside publish() rather than
  // here, because "attached" is the protocol's word and only the backend can
  // answer it without a second round of locking.
  surface_->publish(video);
}

json::Value SharedOutput::status() const {
  json::Value value = json::Value::object();
  value.set("kind", json::Value("shared"));
  value.set("name", json::Value(name_));
  value.set("running", json::Value(running_));
  // Which protocol this build actually speaks. The control page shows it
  // because "shared" alone does not tell an operator what to look for in the
  // other application's source list.
  value.set("protocol", json::Value(sharedSurfaceProtocol()));
  value.set("clients", json::Value(surface_->hasClients()));
  value.set("frames", json::Value(submitted_.load(std::memory_order_relaxed)));
  // published + skipped == frames. The split is the useful part: a source with
  // frames climbing and published stuck at zero is publishing correctly to
  // nobody, which looks identical to a broken output until you can see that
  // skipped is what is moving.
  value.set("published", json::Value(surface_->publishedCount()));
  value.set("skipped", json::Value(surface_->skippedCount()));
  value.set("renderer", json::Value(surface_->describe()));
  return value;
}

std::unique_ptr<IOutput> createSharedOutput(const OutputSpec& spec) {
  return std::make_unique<SharedOutput>(spec);
}

}  // namespace weblinked
