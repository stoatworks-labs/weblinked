#include "engine/engine.h"

#include <algorithm>

#include "diag/diag.h"
#include "outputs/preview_output.h"

namespace weblinked {

Engine::Engine() = default;

Engine::~Engine() { stop(); }

bool Engine::start(const Config& config, std::string& error) {
  if (running_.load()) {
    error = "engine already running";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = config.format;
    matrix_ = config.matrix;
    audioEnabled_ = config.audioEnabled;
  }

  uyvyPool_ = FramePool::create(config.format, PixelFormat::kUYVY, 4);
  blackPool_ = FramePool::create(config.format, PixelFormat::kBGRA, 2);
  straightPool_ = FramePool::create(config.format, PixelFormat::kBGRA, 4);
  blackFrame_ = blackPool_->acquire();
  fillBlackBgra(blackFrame_->data(), blackFrame_->rowBytes(), config.format.width,
                config.format.height);

  browser_ = std::make_unique<BrowserSource>(config.format, &slot_, &audio_);
  browser_->setPacing(config.pacing);
  if (!browser_->open(config.url, error)) {
    return false;
  }

  // Open the outputs. A failure here is recorded against the output and
  // reported, but never fatal: one missing card must not take the IP feeds down.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& spec : config.outputs) {
      Entry entry;
      entry.spec = spec;
      std::string createError;
      entry.output = createOutput(spec, createError);
      if (entry.output == nullptr) {
        diag::error("output '%s' (%s) could not be created: %s", spec.name.c_str(),
                    spec.kind.c_str(), createError.c_str());
        entry.lastError = createError;
        entry.enabled = false;
        outputs_.push_back(std::move(entry));
        continue;
      }
      std::string startError;
      if (!entry.output->start(config.format, startError)) {
        diag::error("output '%s' (%s) failed to start: %s", spec.name.c_str(),
                    spec.kind.c_str(), startError.c_str());
        entry.lastError = startError;
        entry.enabled = false;
      } else {
        diag::info("output '%s' (%s) started", spec.name.c_str(),
                   spec.kind.c_str());
      }
      outputs_.push_back(std::move(entry));
    }
  }

  clock_ = std::make_unique<FrameClock>(config.format.rate);
  running_.store(true);
  clockThread_ = std::thread([this] { clockLoop(); });

  diag::info("engine started: %s at %s", config.url.c_str(),
             config.format.toString().c_str());
  return true;
}

void Engine::stop() {
  if (running_.exchange(false)) {
    // The clock thread may be parked waiting on a format change; wake it or the
    // join below waits forever.
    pauseCv_.notify_all();
    if (clockThread_.joinable()) {
      clockThread_.join();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : outputs_) {
      if (entry.output != nullptr) {
        entry.output->stop();
      }
    }
    outputs_.clear();
  }

  if (browser_ != nullptr) {
    browser_->close();
    browser_.reset();
  }
  clock_.reset();
}

int Engine::audioFramesForTick(int64_t tick) const {
  const int sampleRate = audio_.sampleRate();
  if (sampleRate <= 0) {
    return 0;
  }
  const int64_t numerator = format_.rate.numerator;
  const int64_t denominator = format_.rate.denominator;
  if (numerator <= 0) {
    return 0;
  }
  // Exact positions either side of this tick; the difference is this frame's
  // share, which alternates as it must at fractional rates.
  const int64_t start = (tick * sampleRate * denominator) / numerator;
  const int64_t end = ((tick + 1) * sampleRate * denominator) / numerator;
  return static_cast<int>(end - start);
}

const VideoFrame* Engine::frameInFormat(const VideoFramePtr& source,
                                        PixelFormat target) {
  if (source->pixelFormat() == target) {
    return source.get();
  }
  if (target == PixelFormat::kUYVY) {
    // The pool is sized for the engine's current raster. If the source is a
    // different shape — a paint still in flight from before a format change —
    // refuse rather than convert, because the destination buffer is the wrong
    // size and bgraToUyvy has no way to know that. The caller drops the tick.
    if (!(uyvyPool_->format() == source->format())) {
      return nullptr;
    }
    if (!uyvyScratch_ ||
        !(uyvyScratch_->format() == source->format())) {
      uyvyScratch_ = uyvyPool_->acquire();
    }
    bgraToUyvy(source->data(), source->rowBytes(), uyvyScratch_->data(),
               uyvyScratch_->rowBytes(), source->format().width,
               source->format().height, matrix_);
    uyvyScratch_->setSequence(source->sequence());
    uyvyScratch_->setCaptureNanos(source->captureNanos());
    return uyvyScratch_.get();
  }
  // Only BGRA -> UYVY exists; nothing asks for the reverse.
  return source.get();
}

const VideoFrame* Engine::unpremultiplied(const VideoFramePtr& source) {
  // Same guard as frameInFormat: the pool is sized for the current raster, and a
  // frame still in flight from before a format change is the wrong shape.
  if (!(straightPool_->format() == source->format())) {
    return nullptr;
  }
  if (!straightScratch_ ||
      !(straightScratch_->format() == source->format())) {
    straightScratch_ = straightPool_->acquire();
  }
  unpremultiplyBgra(source->data(), source->rowBytes(), straightScratch_->data(),
                    straightScratch_->rowBytes(), source->format().width,
                    source->format().height);
  straightScratch_->setSequence(source->sequence());
  straightScratch_->setCaptureNanos(source->captureNanos());
  return straightScratch_.get();
}

void Engine::pauseClock() {
  std::unique_lock<std::mutex> lock(pauseMutex_);
  pauseRequested_ = true;
  // Bounded wait: if the clock thread is wedged for some other reason, a control
  // request should fail rather than hang the HTTP server forever. A frame period
  // is the expected wait; a second is generous even at 23.976.
  pauseCv_.wait_for(lock, std::chrono::seconds(1),
                    [this] { return paused_ || !running_.load(); });
}

void Engine::resumeClock() {
  {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    pauseRequested_ = false;
  }
  pauseCv_.notify_all();
}

void Engine::clockLoop() {
  clock_->start();

  VideoFramePtr lastFrame;
  int64_t seenEpoch = formatEpoch_.load();

  while (running_.load()) {
    const int64_t tick = clock_->waitForNextTick();
    if (!running_.load()) {
      break;
    }

    // Stand still if something is rebuilding the state below. Checked here,
    // before anything is touched, which is what makes the rebuild safe.
    {
      std::unique_lock<std::mutex> lock(pauseMutex_);
      if (pauseRequested_) {
        paused_ = true;
        pauseCv_.notify_all();
        pauseCv_.wait(lock, [this] { return !pauseRequested_ || !running_.load(); });
        paused_ = false;
      }
    }
    if (!running_.load()) {
      break;
    }

    const int64_t epoch = formatEpoch_.load();
    if (epoch != seenEpoch) {
      // The raster changed underneath us; the frame we were holding is the wrong
      // shape and must not be handed to an output expecting the new one.
      seenEpoch = epoch;
      lastFrame.reset();
    }

    // Ask for the next paint straight away, so Chromium has a whole frame period
    // to produce it rather than being asked at the moment we need it.
    browser_->requestFrame();

    // peek(), not take(): a page that has not repainted since the last tick — a
    // static graphic, most of the time — must still produce a frame.
    VideoFramePtr frame = slot_.peek();
    if (!frame) {
      frame = blackFrame_;
    } else if (lastFrame && frame->sequence() == lastFrame->sequence()) {
      repeatedFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    lastFrame = frame;
    frame->setCaptureNanos(clock_->elapsedNanos());

    // Prepare audio once for everyone who wants it.
    AudioBlock audio;
    const int wanted = audioEnabled_ ? audioFramesForTick(tick) : 0;
    const int channels = audio_.channels();
    if (wanted > 0 && channels > 0) {
      audioPlanes_.resize(static_cast<size_t>(channels));
      audioPlanePointers_.resize(static_cast<size_t>(channels));
      for (int c = 0; c < channels; ++c) {
        audioPlanes_[static_cast<size_t>(c)].resize(static_cast<size_t>(wanted));
        audioPlanePointers_[static_cast<size_t>(c)] =
            audioPlanes_[static_cast<size_t>(c)].data();
      }
      audio_.readPlanar(audioPlanePointers_.data(), wanted);

      // Interleave from the planes we just read rather than reading the FIFO
      // twice — a second read would consume different samples.
      audioInterleaved_.resize(static_cast<size_t>(wanted) *
                               static_cast<size_t>(channels));
      for (int f = 0; f < wanted; ++f) {
        for (int c = 0; c < channels; ++c) {
          audioInterleaved_[static_cast<size_t>(f) * channels + c] =
              audioPlanes_[static_cast<size_t>(c)][static_cast<size_t>(f)];
        }
      }

      audio.planes = audioPlanePointers_.data();
      audio.interleaved = audioInterleaved_.data();
      audio.frames = wanted;
      audio.channels = channels;
      audio.sampleRate = audio_.sampleRate();
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // A paint from before a resize is still in flight for a frame or two after a
    // format change. It must be dropped *before* anything touches it: the frame
    // pools have already been rebuilt at the new raster, so converting an old
    // 1920x1080 frame into a freshly acquired 1280x720 buffer writes four
    // megabytes past the end of it. That corruption does not fault where it
    // happens — it surfaces a few frames later as a wild jump through a
    // clobbered vtable, which is a genuinely horrible thing to debug.
    if (!(frame->format() == format_)) {
      continue;
    }

    // Convert lazily: only the formats an enabled output actually asked for,
    // and each of them at most once however many outputs want it.
    bool needUyvy = false;
    bool needStraight = false;
    for (const auto& entry : outputs_) {
      if (!entry.enabled || entry.output == nullptr || !entry.output->isRunning()) {
        continue;
      }
      if (entry.output->pixelFormat() == PixelFormat::kUYVY) {
        needUyvy = true;
      } else if (entry.output->wantsStraightAlpha()) {
        needStraight = true;
      }
    }

    const VideoFrame* uyvy =
        needUyvy ? frameInFormat(frame, PixelFormat::kUYVY) : nullptr;
    if (needUyvy && uyvy == nullptr) {
      continue;  // conversion refused; see frameInFormat
    }

    const VideoFrame* straight = needStraight ? unpremultiplied(frame) : nullptr;
    if (needStraight && straight == nullptr) {
      continue;
    }

    for (auto& entry : outputs_) {
      if (!entry.enabled || entry.output == nullptr || !entry.output->isRunning()) {
        continue;
      }
      const VideoFrame* payload = frame.get();
      if (entry.output->pixelFormat() == PixelFormat::kUYVY) {
        payload = uyvy;
      } else if (entry.output->wantsStraightAlpha()) {
        payload = straight;
      }
      if (payload == nullptr) {
        continue;
      }
      entry.output->submit(*payload, entry.output->wantsAudio() ? audio : AudioBlock{});
    }

    ticks_.store(tick, std::memory_order_relaxed);
  }
}

void Engine::setUrl(const std::string& url) {
  if (browser_ != nullptr) {
    browser_->loadUrl(url);
  }
}

void Engine::reload(bool ignoreCache) {
  if (browser_ != nullptr) {
    browser_->reload(ignoreCache);
  }
}

void Engine::runScript(const std::string& script) {
  if (browser_ != nullptr) {
    browser_->executeJavaScript(script);
  }
}

void Engine::setAudioMuted(bool muted) {
  audioMuted_.store(muted);
  if (browser_ != nullptr) {
    browser_->setAudioMuted(muted);
  }
}

bool Engine::setOutputEnabled(const std::string& name, bool enabled,
                              std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : outputs_) {
    if (entry.spec.name != name) {
      continue;
    }
    if (entry.output == nullptr) {
      error = "output '" + name + "' was never created: " + entry.lastError;
      return false;
    }
    if (enabled == entry.enabled && entry.output->isRunning() == enabled) {
      return true;
    }
    if (enabled) {
      std::string startError;
      if (!entry.output->start(format_, startError)) {
        entry.lastError = startError;
        entry.enabled = false;
        error = startError;
        return false;
      }
      entry.lastError.clear();
      entry.enabled = true;
      diag::info("output '%s' enabled", name.c_str());
    } else {
      entry.output->stop();
      entry.enabled = false;
      diag::info("output '%s' disabled", name.c_str());
    }
    return true;
  }
  error = "no output named '" + name + "'";
  return false;
}

bool Engine::addOutput(const OutputSpec& spec, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : outputs_) {
    if (entry.spec.name == spec.name) {
      error = "an output named '" + spec.name + "' already exists";
      return false;
    }
  }

  Entry entry;
  entry.spec = spec;
  entry.output = createOutput(spec, error);
  if (entry.output == nullptr) {
    return false;
  }
  if (!entry.output->start(format_, error)) {
    entry.lastError = error;
    entry.enabled = false;
    outputs_.push_back(std::move(entry));
    return false;
  }
  entry.enabled = true;
  outputs_.push_back(std::move(entry));
  diag::info("output '%s' (%s) added", spec.name.c_str(), spec.kind.c_str());
  return true;
}

bool Engine::removeOutput(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = std::find_if(outputs_.begin(), outputs_.end(),
                               [&](const Entry& entry) {
                                 return entry.spec.name == name;
                               });
  if (it == outputs_.end()) {
    return false;
  }
  if (it->output != nullptr) {
    it->output->stop();
  }
  outputs_.erase(it);
  diag::info("output '%s' removed", name.c_str());
  return true;
}

bool Engine::setFormat(const VideoFormat& format, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (format_ == format) {
      return true;
    }
  }

  // Park the clock thread first. Everything below rebuilds state that thread
  // owns, and the pools in particular cannot be swapped while it is reading
  // them. Scoped, so an early return still releases it.
  ClockPause pause(this);

  std::lock_guard<std::mutex> lock(mutex_);

  // Every output has to be reopened: no SDI device can change mode while
  // running, and the IP senders advertise their raster at creation.
  for (auto& entry : outputs_) {
    if (entry.output != nullptr) {
      entry.output->stop();
    }
  }

  format_ = format;
  formatEpoch_.fetch_add(1);
  uyvyPool_ = FramePool::create(format, PixelFormat::kUYVY, 4);
  blackPool_ = FramePool::create(format, PixelFormat::kBGRA, 2);
  straightPool_ = FramePool::create(format, PixelFormat::kBGRA, 4);
  uyvyScratch_.reset();
  straightScratch_.reset();
  blackFrame_ = blackPool_->acquire();
  fillBlackBgra(blackFrame_->data(), blackFrame_->rowBytes(), format.width,
                format.height);

  if (browser_ != nullptr) {
    browser_->setFormat(format);
  }
  if (clock_ != nullptr) {
    clock_->setRate(format.rate);
  }

  bool allStarted = true;
  for (auto& entry : outputs_) {
    if (entry.output == nullptr || !entry.enabled) {
      continue;
    }
    std::string startError;
    if (!entry.output->start(format, startError)) {
      entry.lastError = startError;
      entry.enabled = false;
      allStarted = false;
      if (error.empty()) {
        error = "'" + entry.spec.name + "': " + startError;
      }
    }
  }
  diag::info("engine raster now %s", format.toString().c_str());
  return allStarted;
}

void Engine::sendInput(const InputEvent& event) {
  if (browser_ == nullptr) {
    return;
  }
  int x = 0;
  int y = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Clamp rather than reject: a drag that leaves the preview should land on
    // the edge of the page, which is what a real pointer would do.
    const double clampedX = std::clamp(event.nx, 0.0, 1.0);
    const double clampedY = std::clamp(event.ny, 0.0, 1.0);
    x = std::clamp(static_cast<int>(clampedX * format_.width), 0,
                   format_.width - 1);
    y = std::clamp(static_cast<int>(clampedY * format_.height), 0,
                   format_.height - 1);
  }
  browser_->sendInput(event, x, y);
}

VideoFormat Engine::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

PreviewOutput* Engine::preview() const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : outputs_) {
    if (auto* view = asPreview(entry.output.get())) {
      return view;
    }
  }
  return nullptr;
}

json::Value Engine::state() const {
  json::Value root = json::Value::object();
  root.set("version", json::Value(WEBLINKED_VERSION));
  root.set("running", json::Value(running_.load()));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    root.set("format", json::Value(format_.toString()));

    json::Value formatDetail = json::Value::object();
    formatDetail.set("width", json::Value(format_.width));
    formatDetail.set("height", json::Value(format_.height));
    formatDetail.set("rate_numerator", json::Value(format_.rate.numerator));
    formatDetail.set("rate_denominator", json::Value(format_.rate.denominator));
    formatDetail.set("interlaced", json::Value(format_.interlaced));
    root.set("format_detail", formatDetail);

    json::Value outputs = json::Value::array();
    for (const auto& entry : outputs_) {
      json::Value value = entry.output != nullptr ? entry.output->status()
                                                  : json::Value::object();
      value.set("name", json::Value(entry.spec.name));
      value.set("kind", json::Value(entry.spec.kind));
      value.set("enabled", json::Value(entry.enabled));
      if (!entry.lastError.empty()) {
        value.set("error", json::Value(entry.lastError));
      }
      outputs.push(value);
    }
    root.set("outputs", outputs);
  }

  json::Value compiled = json::Value::array();
  for (const auto& kind : compiledOutputKinds()) {
    compiled.push(json::Value(kind));
  }
  root.set("compiled_backends", compiled);

  if (browser_ != nullptr) {
    const auto diagnostics = browser_->diagnostics();
    json::Value source = json::Value::object();
    source.set("url", json::Value(browser_->url()));
    source.set("loaded_url", json::Value(diagnostics.url));
    source.set("loading", json::Value(diagnostics.loading));
    source.set("paints", json::Value(diagnostics.paints));
    source.set("audio_packets", json::Value(diagnostics.audioPackets));
    source.set("console_errors", json::Value(diagnostics.consoleErrors));
    source.set("audio_muted", json::Value(audioMuted_.load()));
    source.set("pacing",
               json::Value(browser_->pacing() ==
                                   BrowserSource::Pacing::kExternalBeginFrame
                               ? "external"
                               : "internal"));
    if (!diagnostics.lastError.empty()) {
      source.set("last_error", json::Value(diagnostics.lastError));
    }
    if (!diagnostics.lastConsoleError.empty()) {
      source.set("last_console_error", json::Value(diagnostics.lastConsoleError));
    }
    root.set("source", source);
  }

  json::Value pacing = json::Value::object();
  pacing.set("ticks", json::Value(ticks_.load()));
  pacing.set("repeated_frames", json::Value(repeatedFrames_.load()));
  pacing.set("frames_published", json::Value(slot_.publishedCount()));
  pacing.set("frames_overwritten", json::Value(slot_.overwrittenCount()));
  if (clock_ != nullptr) {
    pacing.set("dropped_ticks", json::Value(clock_->droppedTicks()));
    pacing.set("last_lateness_us",
               json::Value(clock_->lastLatenessNanos() / 1000));
  }
  root.set("pacing", pacing);

  const auto audioStats = audio_.stats();
  json::Value audio = json::Value::object();
  audio.set("channels", json::Value(audio_.channels()));
  audio.set("sample_rate", json::Value(audio_.sampleRate()));
  audio.set("buffered_frames", json::Value(audio_.availableFrames()));
  audio.set("underruns", json::Value(audioStats.underruns));
  audio.set("overruns", json::Value(audioStats.overruns));
  root.set("audio", audio);

  return root;
}

}  // namespace weblinked
