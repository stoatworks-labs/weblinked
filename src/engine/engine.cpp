#include "engine/engine.h"

#include <algorithm>

#include "core/source_config.h"
#include "diag/diag.h"
#include "outputs/preview_output.h"
#if defined(WEBLINKED_WITH_SCREEN)
#include "outputs/screen_window.h"
#endif

namespace weblinked {

namespace {

using Clock = std::chrono::steady_clock;

/// How long state() waits for mutex_ before answering with the last report.
/// mutex_ is never held across a driver call, so this is belt and braces.
constexpr auto kStateLockWait = std::chrono::milliseconds(100);

/// One state() call's budget for every output's status() together. A client
/// polling every 200 ms with a 300 ms timeout — what Daniel's supervisor does —
/// gets an answer inside it even with an output wedged.
constexpr auto kStatusBudget = std::chrono::milliseconds(100);

/// How long the clock thread waits, per tick and for all outputs together, for
/// a gate held by someone else. Normally that is a status() taking a few
/// microseconds; an output being restarted is disabled first, so the clock is
/// not waiting on its start().
constexpr auto kClockGateBudget = std::chrono::milliseconds(5);

/// Control paths: how long to wait behind another control request on the same
/// source, and for an output's gate. Long enough for a card to open; short
/// enough that a request behind a wedged driver fails while someone is still
/// looking at it.
constexpr auto kControlWait = std::chrono::seconds(5);
constexpr auto kGateWait = std::chrono::seconds(2);

/// The preview is read a few times a second by the control page.
constexpr auto kPreviewWait = std::chrono::milliseconds(250);

int64_t steadyNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

const char* kControlBusy =
    "another change to this source's outputs is still in progress — if this "
    "persists, an output's driver may have stopped answering (see the "
    "\"stale\" outputs in /api/state)";

}  // namespace

Engine::Engine() = default;

Engine::~Engine() { stop(); }

bool Engine::start(const Config& config, std::string& error) {
  if (running_.load()) {
    error = "engine already running";
    return false;
  }

  if (!config.id.empty()) {
    id_ = config.id;
  }

  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    format_ = config.format;
    matrix_ = config.matrix;
    audioEnabled_ = config.audioEnabled;
  }

  uyvyPool_ = FramePool::create(config.format, PixelFormat::kUYVY, 4);
  blackPool_ = FramePool::create(config.format, PixelFormat::kBGRA, 2);
  straightPool_ = FramePool::create(config.format, PixelFormat::kBGRA, 4);
  backgroundPool_ = FramePool::create(config.format, PixelFormat::kBGRA, 4);
  blackFrame_ = blackPool_->acquire();
  fillBlackBgra(blackFrame_->data(), blackFrame_->rowBytes(), config.format.width,
                config.format.height);
  // Paints are numbered from 0, so the black stand-in sent before the first
  // one must not also be 0: an output that skips frames it has already drawn
  // (kms) would take the first real paint for a repeat of the black and never
  // show it. -1 is "no paint yet", and the composited variants inherit it.
  blackFrame_->setSequence(-1);

  browser_ = std::make_unique<BrowserSource>(config.format, &slot_, &audio_);
  browser_->setPacing(config.pacing);
  browser_->setPopupPolicy(config.popupPolicy);
  interactiveByDefault_.store(config.interactiveByDefault);
  if (!browser_->open(config.url, error)) {
    return false;
  }

  // Open the outputs. A failure here is recorded against the output and
  // reported, but never fatal: one missing card must not take the IP feeds down.
  //
  // Opened before they are published under mutex_, so that no driver call is
  // ever made holding it — not even here, where no other thread can see this
  // engine yet. A rule with no exceptions is one that can be checked by grep.
  std::vector<Entry> opened;
  for (const auto& spec : config.outputs) {
    Entry entry;
    entry.spec = spec;
    std::string createError;
    auto output = createOutput(spec, createError);
    if (output == nullptr) {
      diag::error("output '%s' (%s) could not be created: %s", spec.name.c_str(),
                  spec.kind.c_str(), createError.c_str());
      entry.lastError = createError;
      entry.enabled = false;
    } else {
      std::string startError;
      if (!output->start(config.format, startError)) {
        diag::error("output '%s' (%s) failed to start: %s", spec.name.c_str(),
                    spec.kind.c_str(), startError.c_str());
        entry.lastError = startError;
        entry.enabled = false;
      } else {
        diag::info("output '%s' (%s) started", spec.name.c_str(),
                   spec.kind.c_str());
      }
    }
    entry.slot = std::make_shared<OutputSlot>(std::move(output));
    entry.desc = entry.slot->description();
    opened.push_back(std::move(entry));
  }
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    outputs_ = std::move(opened);
  }

  clock_ = std::make_unique<FrameClock>(config.format.rate);
  running_.store(true);
  clockThread_ = std::thread([this] { clockLoop(); });

  diag::info("engine started: %s at %s", config.url.c_str(),
             config.format.toString().c_str());
  return true;
}

void Engine::halt() {
  if (running_.exchange(false)) {
    // The clock thread may be parked waiting on a format change; wake it or the
    // join below waits forever. Notified under the lock: unlocked, the wake-up
    // could land between the clock thread testing running_ and going to sleep,
    // and be lost.
    {
      std::lock_guard<std::mutex> lock(pauseMutex_);
      pauseCv_.notify_all();
    }
    if (clockThread_.joinable()) {
      clockThread_.join();
    }
  }

  // Behind any control request already in flight, but not for ever: this is
  // the shutdown path, and it has a watchdog waiting on it.
  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kGateWait)) {
    diag::warn("engine '%s': halting with a control request still in progress",
               id_.c_str());
  }

  std::vector<Entry> entries;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    entries.swap(outputs_);
  }
  for (auto& entry : entries) {
    if (!entry.slot->exclusive(Clock::now() + kGateWait,
                               [](std::unique_ptr<IOutput>& output) {
                                 if (output != nullptr) {
                                   output->stop();
                                   output.reset();
                                 }
                               })) {
      // Never destroyed under a call in flight: the slot keeps it, and it goes
      // when whoever is inside the driver lets go of their copy.
      diag::error("output '%s' did not stop: a call into its driver has been "
                  "running for %lld ms",
                  entry.spec.name.c_str(),
                  static_cast<long long>(entry.slot->busyMillis()));
    }
  }
}

void Engine::stop() {
  halt();

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
               source->format().height, matrix_.load());
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

Engine::BackgroundVariant& Engine::backgroundVariant(uint32_t colour) {
  const auto it = std::find_if(backgrounds_.begin(), backgrounds_.end(),
                               [&](const BackgroundVariant& variant) {
                                 return variant.colour == colour;
                               });
  if (it != backgrounds_.end()) {
    return *it;
  }
  BackgroundVariant variant;
  variant.colour = colour;
  backgrounds_.push_back(std::move(variant));
  return backgrounds_.back();
}

const VideoFrame* Engine::compositedBgra(const VideoFramePtr& source,
                                         uint32_t colour, int64_t tick) {
  // Same guard as frameInFormat and unpremultiplied: the pool is sized for the
  // current raster, and a paint still in flight from before a format change is
  // the wrong shape.
  if (!(backgroundPool_->format() == source->format())) {
    return nullptr;
  }
  BackgroundVariant& variant = backgroundVariant(colour);
  if (variant.bgraTick == tick && variant.bgra) {
    return variant.bgra.get();
  }
  if (!variant.bgra || !(variant.bgra->format() == source->format())) {
    variant.bgra = backgroundPool_->acquire();
  }
  compositeOverBgra(source->data(), source->rowBytes(), variant.bgra->data(),
                    variant.bgra->rowBytes(), source->format().width,
                    source->format().height,
                    static_cast<uint8_t>(colour & 0xff),
                    static_cast<uint8_t>((colour >> 8) & 0xff),
                    static_cast<uint8_t>((colour >> 16) & 0xff));
  variant.bgra->setSequence(source->sequence());
  variant.bgra->setCaptureNanos(source->captureNanos());
  variant.bgraTick = tick;
  return variant.bgra.get();
}

const VideoFrame* Engine::compositedUyvy(const VideoFramePtr& source,
                                         uint32_t colour, int64_t tick) {
  const VideoFrame* bgra = compositedBgra(source, colour, tick);
  if (bgra == nullptr) {
    return nullptr;
  }
  BackgroundVariant& variant = backgroundVariant(colour);
  if (variant.uyvyTick == tick && variant.uyvy) {
    return variant.uyvy.get();
  }
  if (!variant.uyvy || !(variant.uyvy->format() == bgra->format())) {
    variant.uyvy = uyvyPool_->acquire();
  }
  bgraToUyvy(bgra->data(), bgra->rowBytes(), variant.uyvy->data(),
             variant.uyvy->rowBytes(), bgra->format().width,
             bgra->format().height, matrix_.load());
  variant.uyvy->setSequence(bgra->sequence());
  variant.uyvy->setCaptureNanos(bgra->captureNanos());
  variant.uyvyTick = tick;
  return variant.uyvy.get();
}

bool Engine::pauseClock() {
  std::unique_lock<std::mutex> lock(pauseMutex_);
  pauseRequested_ = true;
  // Bounded wait: if the clock thread is wedged for some other reason, a control
  // request should fail rather than hang the HTTP server forever. A frame period
  // is the expected wait; a second is generous even at 23.976.
  return pauseCv_.wait_for(lock, std::chrono::seconds(1),
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
    lastTickNanos_.store(steadyNanos());

    const int64_t epoch = formatEpoch_.load();
    if (epoch != seenEpoch) {
      // The raster changed underneath us; the frame we were holding is the wrong
      // shape and must not be handed to an output expecting the new one.
      seenEpoch = epoch;
      lastFrame.reset();
    }

    // peek(), not take(): a page that has not repainted since the last tick — a
    // static graphic, most of the time — must still produce a frame.
    //
    // Read *before* asking for the next paint, never after. Asking first let a
    // fast paint race the read: when the clock thread lost the CPU for a couple
    // of milliseconds after posting the request — routine on a two-core
    // machine — it came back to the paint it had just asked for, and on other
    // ticks to the one before. Every flip between the two held one frame or
    // skipped one. Read first, and what goes out is always the paint requested
    // one tick ago. See §34 of docs/04-verification.md.
    VideoFramePtr frame = slot_.peek();

    // Then ask for the next paint straight away, so Chromium has a whole frame
    // period to produce it rather than being asked at the moment we need it.
    browser_->requestFrame();

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

    // Which outputs this tick goes to: copied out under mutex_, and called after
    // it is released. Holding it across submit() is how a DeckLink driver that
    // deadlocked inside the SDK took every control request down with it (#12).
    targets_.clear();
    bool staleRaster = false;
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      // A paint from before a resize is still in flight for a frame or two
      // after a format change. It must be dropped *before* anything touches it:
      // the frame pools have already been rebuilt at the new raster, so
      // converting an old 1920x1080 frame into a freshly acquired 1280x720
      // buffer writes four megabytes past the end of it. That corruption does
      // not fault where it happens — it surfaces a few frames later as a wild
      // jump through a clobbered vtable, which is a genuinely horrible thing to
      // debug.
      staleRaster = !(frame->format() == format_);
      if (!staleRaster) {
        for (const auto& entry : outputs_) {
          if (entry.enabled && entry.desc.present) {
            targets_.push_back(Target{entry.slot, entry.desc, entry.spec.background});
          }
        }
      }
    }
    if (staleRaster) {
      continue;
    }

    // Convert lazily: only the formats an enabled output actually asked for,
    // and each of them at most once however many outputs want it.
    //
    // An output compositing over a background needs neither of these: the
    // composite is opaque by definition, which makes un-premultiplying a no-op
    // and gives the UYVY converter a different source frame to work from.
    bool needUyvy = false;
    bool needStraight = false;
    backgroundKeys_.clear();
    for (const auto& target : targets_) {
      if (target.background.opaque) {
        const uint32_t key = target.background.packed();
        if (std::find(backgroundKeys_.begin(), backgroundKeys_.end(), key) ==
            backgroundKeys_.end()) {
          backgroundKeys_.push_back(key);
        }
      } else if (target.desc.pixelFormat == PixelFormat::kUYVY) {
        needUyvy = true;
      } else if (target.desc.straightAlpha) {
        needStraight = true;
      }
    }

    // Forget the buffers for any colour nothing wants any more. Cheap — the
    // list is one or two entries — and it is what stops a colour picker being
    // dragged from parking a raster-sized pair of frames per shade it passed.
    std::erase_if(backgrounds_, [this](const BackgroundVariant& variant) {
      return std::find(backgroundKeys_.begin(), backgroundKeys_.end(),
                       variant.colour) == backgroundKeys_.end();
    });

    const VideoFrame* uyvy =
        needUyvy ? frameInFormat(frame, PixelFormat::kUYVY) : nullptr;
    if (needUyvy && uyvy == nullptr) {
      targets_.clear();
      continue;  // conversion refused; see frameInFormat
    }

    const VideoFrame* straight = needStraight ? unpremultiplied(frame) : nullptr;
    if (needStraight && straight == nullptr) {
      targets_.clear();
      continue;
    }

    // One budget for the whole tick: an output whose gate is held — normally
    // by a status() lasting microseconds — gets a few milliseconds, and one
    // whose driver has been stuck for a second is tried once and skipped. The
    // others still get their frame either way.
    const auto gateDeadline = Clock::now() + kClockGateBudget;
    for (const auto& target : targets_) {
      const VideoFrame* payload = frame.get();
      if (target.background.opaque) {
        // The composite comes out opaque, so a keyed output gets a solid key
        // and a straight-alpha one needs no un-premultiplying: at alpha 255 the
        // two representations are the same bytes.
        const uint32_t colour = target.background.packed();
        payload = target.desc.pixelFormat == PixelFormat::kUYVY
                      ? compositedUyvy(frame, colour, tick)
                      : compositedBgra(frame, colour, tick);
      } else if (target.desc.pixelFormat == PixelFormat::kUYVY) {
        payload = uyvy;
      } else if (target.desc.straightAlpha) {
        payload = straight;
      }
      if (payload == nullptr) {
        continue;
      }
      // The generation check inside call() is what makes the plan above safe
      // to act on outside the lock: if the output was replaced since — a UYVY
      // one by a BGRA one, say — the payload is the wrong size for it, and the
      // call does nothing.
      const AudioBlock block = target.desc.wantsAudio ? audio : AudioBlock{};
      if (!target.slot->call(gateDeadline, target.desc.generation,
                              [&](IOutput& output) {
                                if (output.isRunning()) {
                                  output.submit(*payload, block);
                                }
                              })) {
        outputSkips_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    targets_.clear();

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

void Engine::setPacing(BrowserSource::Pacing pacing) {
  if (browser_ != nullptr) {
    browser_->setPacing(pacing);
  }
}

void Engine::setPopupPolicy(RenderClient::PopupPolicy policy) {
  if (browser_ != nullptr) {
    browser_->setPopupPolicy(policy);
  }
}

void Engine::setMatrix(ColourMatrix matrix) { matrix_.store(matrix); }

ColourMatrix Engine::matrix() const { return matrix_.load(); }

std::vector<OutputSpec> Engine::outputSpecs() const {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  std::vector<OutputSpec> specs;
  specs.reserve(outputs_.size());
  for (const auto& entry : outputs_) {
    specs.push_back(entry.spec);
  }
  return specs;
}

SourceConfig Engine::configuration() const {
  SourceConfig config;
  config.id = id_;
  config.url = browser_ != nullptr ? browser_->url() : std::string("about:blank");
  config.matrix = matrix_.load();
  config.externalPacing =
      browser_ == nullptr ||
      browser_->pacing() == BrowserSource::Pacing::kExternalBeginFrame;
  config.interactiveByDefault = interactiveByDefault_.load();
  config.popupPolicy =
      browser_ != nullptr &&
              browser_->popupPolicy() == RenderClient::PopupPolicy::kBlock
          ? "block"
          : "navigate";
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    config.format = format_;
    config.audioEnabled = audioEnabled_;
    for (const auto& entry : outputs_) {
      config.outputs.push_back(outputConfigFromSpec(entry.spec));
    }
  }
  // Whether a preview is present is a property of the list we just captured;
  // asking for one to be added on load would duplicate it.
  config.wantPreview = false;
  return config;
}

bool Engine::applyConfiguration(const SourceConfig& config, std::string& error) {
  std::string firstError;
  const auto note = [&firstError](const std::string& message) {
    if (firstError.empty()) {
      firstError = message;
    }
  };

  if (!(config.format == format())) {
    std::string formatError;
    if (!setFormat(config.format, formatError)) {
      note(formatError);
    }
  }

  setMatrix(config.matrix);
  setPacing(config.externalPacing ? BrowserSource::Pacing::kExternalBeginFrame
                                  : BrowserSource::Pacing::kInternalTimer);
  setInteractiveByDefault(config.interactiveByDefault);
  setPopupPolicy(config.popupPolicy == "block"
                     ? RenderClient::PopupPolicy::kBlock
                     : RenderClient::PopupPolicy::kNavigateInPlace);

  // Outputs the configuration does not mention go first, so a card being handed
  // from one output to another is released before the new one claims it.
  for (const auto& spec : outputSpecs()) {
    const bool wanted =
        std::any_of(config.outputs.begin(), config.outputs.end(),
                    [&](const OutputConfig& candidate) {
                      return candidate.name == spec.name;
                    });
    if (!wanted) {
      removeOutput(spec.name);
    }
  }

  for (const auto& outputConfig : config.outputs) {
    const OutputSpec spec = outputSpecFromConfig(outputConfig);
    const auto existing = outputSpecs();
    const bool present = std::any_of(existing.begin(), existing.end(),
                                     [&](const OutputSpec& candidate) {
                                       return candidate.name == spec.name;
                                     });
    std::string outputError;
    if (present) {
      // An output whose settings are unchanged is left alone — restarting it
      // would drop frames on air for no reason.
      const auto same = std::find_if(existing.begin(), existing.end(),
                                     [&](const OutputSpec& candidate) {
                                       return candidate.name == spec.name;
                                     });
      if (same->kind == spec.kind && same->deviceIndex == spec.deviceIndex &&
          same->background == spec.background &&
          same->options.serialize() == spec.options.serialize()) {
        continue;
      }
      // A background-only difference falls through to updateOutput, which
      // recognises it and assigns rather than reopening the device.
      if (!updateOutput(spec.name, spec, outputError)) {
        note(outputError);
      }
    } else if (!addOutput(spec, outputError)) {
      note(outputError);
    }
  }

  if (config.url != (browser_ != nullptr ? browser_->url() : std::string{})) {
    setUrl(config.url);
  }

  // `audioEnabled` is deliberately not applied here. It decides whether audio is
  // captured and prepared at all, which the clock thread reads on every tick,
  // and it is not the same thing as the runtime mute the control page offers.
  // A saved configuration changes it for the next launch.

  error = firstError;
  return firstError.empty();
}

Engine::Entry* Engine::findEntry(const std::string& name) {
  const auto it = std::find_if(outputs_.begin(), outputs_.end(),
                               [&](const Entry& entry) {
                                 return entry.spec.name == name;
                               });
  return it == outputs_.end() ? nullptr : &*it;
}

std::string Engine::busyMessage(const std::string& name, const OutputSlot& slot) {
  const int64_t busy = slot.busyMillis();
  std::string message = "output '" + name + "' is not answering";
  if (busy > 0) {
    message += ": a call into its driver has been running for " +
               std::to_string(busy / 1000) + "." +
               std::to_string((busy % 1000) / 100) + " s";
  }
  return message + "; nothing was changed";
}

bool Engine::setOutputEnabled(const std::string& name, bool enabled,
                              std::string& error) {
  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kControlWait)) {
    error = kControlBusy;
    return false;
  }

  std::shared_ptr<OutputSlot> slot;
  VideoFormat format;
  bool wasEnabled = false;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    Entry* entry = findEntry(name);
    if (entry == nullptr) {
      error = "no output named '" + name + "'";
      return false;
    }
    if (!entry->desc.present) {
      error = "output '" + name + "' was never created: " + entry->lastError;
      return false;
    }
    slot = entry->slot;
    format = format_;
    wasEnabled = entry->enabled;
    // Stop feeding it before stopping it, so the clock is not left waiting on
    // the gate behind a stop() that may take a while.
    if (!enabled) {
      entry->enabled = false;
    }
  }

  bool acted = false;
  bool started = true;
  std::string startError;
  const bool ran = slot->exclusive(
      Clock::now() + kGateWait, [&](std::unique_ptr<IOutput>& output) {
        if (output == nullptr) {
          started = false;
          startError = "output '" + name + "' has no device";
          return;
        }
        if (enabled == wasEnabled && output->isRunning() == enabled) {
          return;
        }
        acted = true;
        if (enabled) {
          started = output->start(format, startError);
        } else {
          output->stop();
        }
      });

  std::lock_guard<std::timed_mutex> lock(mutex_);
  Entry* entry = findEntry(name);
  if (!ran) {
    error = busyMessage(name, *slot);
    if (entry != nullptr) {
      entry->enabled = wasEnabled;
    }
    diag::warn("%s", error.c_str());
    return false;
  }
  if (entry == nullptr) {
    error = "output '" + name + "' was removed while it was being changed";
    return false;
  }
  entry->desc = slot->description();
  if (!enabled) {
    entry->enabled = false;
    if (acted) {
      diag::info("output '%s' disabled", name.c_str());
    }
    return true;
  }
  if (!started) {
    entry->lastError = startError;
    entry->enabled = false;
    error = startError;
    return false;
  }
  entry->lastError.clear();
  entry->enabled = true;
  if (acted) {
    diag::info("output '%s' enabled", name.c_str());
  }
  return true;
}

bool Engine::addOutput(const OutputSpec& spec, std::string& error) {
  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kControlWait)) {
    error = kControlBusy;
    return false;
  }

  VideoFormat format;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    for (const auto& entry : outputs_) {
      if (entry.spec.name == spec.name) {
        error = "an output named '" + spec.name + "' already exists";
        return false;
      }
    }
    // One preview, no more. Not a duplicate of the name check above: a second
    // preview under a different name costs a scale-and-copy per tick and the
    // control page would only ever show one of them. Conditional on there being
    // one already rather than refusing the kind outright, so a source that
    // somehow lost its preview can still be given one back.
    if (outputKindIsPermanent(spec.kind)) {
      for (const auto& entry : outputs_) {
        if (outputKindIsPermanent(entry.spec.kind)) {
          error = "there is already a preview output";
          return false;
        }
      }
    }
    format = format_;
  }

  // Created and opened before anyone else can see it, so with no lock at all.
  // The name stays reserved meanwhile because every other path that could add
  // one is waiting on controlMutex_.
  auto output = createOutput(spec, error);
  if (output == nullptr) {
    return false;
  }
  Entry entry;
  entry.spec = spec;
  const bool started = output->start(format, error);
  entry.slot = std::make_shared<OutputSlot>(std::move(output));
  entry.desc = entry.slot->description();
  entry.enabled = started;
  if (!started) {
    entry.lastError = error;
  }
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    outputs_.push_back(std::move(entry));
  }
  if (started) {
    diag::info("output '%s' (%s) added", spec.name.c_str(), spec.kind.c_str());
  }
  return started;
}

bool Engine::setOutputBackground(const std::string& name,
                                 const OutputBackground& background,
                                 std::string& error) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  for (auto& entry : outputs_) {
    if (entry.spec.name != name) {
      continue;
    }
    entry.spec.background = background;
    diag::info("output '%s' background now %s", name.c_str(),
               background.opaque ? background.toHex().c_str() : "transparent");
    return true;
  }
  error = "no output named '" + name + "'";
  return false;
}

bool Engine::updateOutput(const std::string& name, const OutputSpec& spec,
                          std::string& error) {
  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kControlWait)) {
    error = kControlBusy;
    return false;
  }

  std::shared_ptr<OutputSlot> slot;
  OutputSlot::Description desc;
  OutputSpec previousSpec;
  bool wasEnabled = false;
  VideoFormat format;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    Entry* entry = findEntry(name);
    if (entry == nullptr) {
      error = "no output named '" + name + "'";
      return false;
    }

    // A permanent output keeps its kind. This is the edit that silently kills
    // the control page: converting the preview row into an NDI feed succeeds,
    // the feed goes on air, and the only visible consequence is a preview pane
    // that is black from then on. Everything else about the preview — its
    // scale, its background — stays editable, so this refuses the one change
    // that cannot be undone from the page it breaks.
    if (outputKindIsPermanent(entry->spec.kind) && spec.kind != entry->spec.kind) {
      error = "the preview cannot become a '" + spec.kind +
              "' output — add a second output instead";
      return false;
    }
    // And nothing else may turn into one, which would leave two previews with
    // the control page reading whichever it found first.
    if (!outputKindIsPermanent(entry->spec.kind) &&
        outputKindIsPermanent(spec.kind)) {
      error = "there is already a preview output";
      return false;
    }
    if (spec.name != name) {
      for (const auto& other : outputs_) {
        if (other.spec.name == spec.name) {
          error = "an output named '" + spec.name + "' already exists";
          return false;
        }
      }
    }
    slot = entry->slot;
    desc = entry->desc;
    previousSpec = entry->spec;
    wasEnabled = entry->enabled;
    format = format_;
  }

  // Nothing the device cares about has moved. Assigning the background and
  // returning is not an optimisation: reopening a DeckLink to change a colour
  // the engine composites in software would drop frames on air, and pressing
  // Apply having changed nothing at all would do the same.
  //
  // Only for an output that is actually running, though — on a broken one,
  // Apply is how an operator retries a card that was not plugged in yet, and
  // that has to keep reaching the code below. Running is asked through the
  // gate; an output that cannot answer is treated as not running, and the
  // reopen below then reports it busy.
  bool running = false;
  if (desc.present) {
    slot->call(Clock::now() + kGateWait, desc.generation,
               [&](IOutput& output) { running = output.isRunning(); });
  }
  if (running && spec.name == name && spec.kind == previousSpec.kind &&
      spec.deviceIndex == previousSpec.deviceIndex &&
      spec.options.serialize() == previousSpec.options.serialize()) {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    Entry* entry = findEntry(name);
    if (entry == nullptr) {
      error = "no output named '" + name + "'";
      return false;
    }
    if (entry->spec.background != spec.background) {
      diag::info("output '%s' background now %s", name.c_str(),
                 spec.background.opaque ? spec.background.toHex().c_str()
                                        : "transparent");
    }
    entry->spec.background = spec.background;
    return true;
  }

  // Stop feeding it while it is replaced.
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (Entry* entry = findEntry(name)) {
      entry->enabled = false;
    }
  }

  // The old device has to be released before the new one is opened: on a rename
  // of the same DeckLink or the same NDI name, the two would otherwise be
  // fighting over one resource and the new one would simply fail. All of it
  // under the slot's gate, so the clock never sees a half-replaced output.
  bool replaced = false;
  bool restored = false;
  std::string restoreError;
  const bool ran = slot->exclusive(
      Clock::now() + kGateWait, [&](std::unique_ptr<IOutput>& output) {
        if (output != nullptr) {
          output->stop();
          output.reset();
        }
        auto replacement = createOutput(spec, error);
        std::string startError;
        if (replacement != nullptr && replacement->start(format, startError)) {
          output = std::move(replacement);
          replaced = true;
          return;
        }
        if (replacement != nullptr) {
          error = startError;
        }
        // Put back what was there. If even that fails the entry keeps its
        // original spec and carries the error, so the settings page shows the
        // output as broken rather than silently dropping it.
        output = createOutput(previousSpec, restoreError);
        if (output != nullptr && wasEnabled) {
          restored = output->start(format, restoreError);
        }
      });

  std::lock_guard<std::timed_mutex> lock(mutex_);
  Entry* entry = findEntry(name);
  if (!ran) {
    error = busyMessage(name, *slot);
    if (entry != nullptr) {
      entry->enabled = wasEnabled;
    }
    diag::warn("%s", error.c_str());
    return false;
  }
  if (entry == nullptr) {
    error = "output '" + name + "' was removed while it was being changed";
    return false;
  }
  entry->desc = slot->description();
  if (replaced) {
    entry->spec = spec;
    entry->enabled = true;
    entry->lastError.clear();
    diag::info("output '%s' updated (now '%s', %s)", name.c_str(),
               spec.name.c_str(), spec.kind.c_str());
    return true;
  }
  entry->enabled = restored;
  entry->lastError = error;
  if (wasEnabled && !restored) {
    diag::error("output '%s' could not be restored after a failed update: %s",
                name.c_str(), restoreError.c_str());
  }
  diag::warn("output '%s' update rejected: %s", name.c_str(), error.c_str());
  return false;
}

bool Engine::removeOutput(const std::string& name, std::string* error) {
  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kControlWait)) {
    if (error != nullptr) {
      *error = kControlBusy;
    }
    return false;
  }

  std::shared_ptr<OutputSlot> slot;
  bool wasEnabled = false;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    Entry* entry = findEntry(name);
    if (entry == nullptr) {
      if (error != nullptr) {
        *error = "no output named '" + name + "'";
      }
      return false;
    }
    // Permanent outputs stay. applyConfiguration reaches here too, which is the
    // point: a settings file written before this rule existed has no preview in
    // its list, and reconciling against it must not delete the running one.
    if (outputKindIsPermanent(entry->spec.kind)) {
      if (error != nullptr) {
        *error = "the preview cannot be removed — it is what the control page shows";
      }
      return false;
    }
    slot = entry->slot;
    wasEnabled = entry->enabled;
    entry->enabled = false;  // stop feeding it first
  }

  // Stopped and destroyed under its gate, so never under a call in flight. If
  // the gate cannot be had the output stays in the list, as it was: removing
  // the entry would leave a device open with nothing left that could close it.
  const bool ran = slot->exclusive(Clock::now() + kGateWait,
                                   [](std::unique_ptr<IOutput>& output) {
                                     if (output != nullptr) {
                                       output->stop();
                                       output.reset();
                                     }
                                   });

  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!ran) {
    const std::string message = busyMessage(name, *slot);
    if (Entry* entry = findEntry(name)) {
      entry->enabled = wasEnabled;
    }
    if (error != nullptr) {
      *error = message;
    }
    diag::warn("%s", message.c_str());
    return false;
  }
  const auto it = std::find_if(outputs_.begin(), outputs_.end(),
                               [&](const Entry& entry) {
                                 return entry.spec.name == name;
                               });
  if (it != outputs_.end()) {
    outputs_.erase(it);
  }
  diag::info("output '%s' removed", name.c_str());
  return true;
}

bool Engine::setFormat(const VideoFormat& format, std::string& error) {
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (format_ == format) {
      return true;
    }
  }

  std::unique_lock<std::timed_mutex> control(controlMutex_, std::defer_lock);
  if (!control.try_lock_for(kControlWait)) {
    error = kControlBusy;
    return false;
  }

  // Park the clock thread first. Everything below rebuilds state that thread
  // owns, and the pools in particular cannot be swapped while it is reading
  // them. Scoped, so an early return still releases it.
  ClockPause pause(this);
  if (!pause.parked()) {
    // The clock did not stop within a second, which means it is stuck inside
    // something — an output's driver, most likely. Rebuilding its pools
    // underneath it would be the heap overflow this pause exists to prevent.
    error = "the format was not changed: the clock did not stop for it, which "
            "usually means an output's driver is not answering";
    diag::error("%s", error.c_str());
    return false;
  }

  struct Item {
    std::string name;
    std::shared_ptr<OutputSlot> slot;
    bool enabled = false;
    bool stopped = false;
    bool started = false;
    std::string startError;
  };
  std::vector<Item> items;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    for (const auto& entry : outputs_) {
      items.push_back(Item{entry.spec.name, entry.slot, entry.enabled});
    }
  }

  // Every output has to be reopened: no SDI device can change mode while
  // running, and the IP senders advertise their raster at creation. Each one
  // under its own gate, and without mutex_, so state() keeps answering through
  // what can be several seconds of cards reopening.
  for (auto& item : items) {
    item.stopped = item.slot->exclusive(Clock::now() + kGateWait,
                                        [](std::unique_ptr<IOutput>& output) {
                                          if (output != nullptr) {
                                            output->stop();
                                          }
                                        });
  }

  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    format_ = format;
  }
  formatEpoch_.fetch_add(1);
  uyvyPool_ = FramePool::create(format, PixelFormat::kUYVY, 4);
  blackPool_ = FramePool::create(format, PixelFormat::kBGRA, 2);
  straightPool_ = FramePool::create(format, PixelFormat::kBGRA, 4);
  backgroundPool_ = FramePool::create(format, PixelFormat::kBGRA, 4);
  uyvyScratch_.reset();
  straightScratch_.reset();
  // Every composited frame belongs to the old raster. Dropping the variants
  // rather than the buffers inside them keeps the "wrong shape" case impossible
  // instead of merely guarded.
  backgrounds_.clear();
  blackFrame_ = blackPool_->acquire();
  fillBlackBgra(blackFrame_->data(), blackFrame_->rowBytes(), format.width,
                format.height);
  blackFrame_->setSequence(-1);  // "no paint yet" — see the constructor

  if (browser_ != nullptr) {
    browser_->setFormat(format);
  }
  if (clock_ != nullptr) {
    clock_->setRate(format.rate);
  }

  for (auto& item : items) {
    if (!item.enabled || !item.stopped) {
      continue;
    }
    item.slot->exclusive(Clock::now() + kGateWait,
                         [&](std::unique_ptr<IOutput>& output) {
                           if (output == nullptr) {
                             item.startError = "output has no device";
                             return;
                           }
                           item.started = output->start(format, item.startError);
                         });
  }

  bool allStarted = true;
  std::lock_guard<std::timed_mutex> lock(mutex_);
  for (auto& item : items) {
    Entry* entry = findEntry(item.name);
    if (entry == nullptr) {
      continue;
    }
    entry->desc = item.slot->description();
    if (!item.stopped) {
      // Still open at the old raster, so it must not be fed the new one: the
      // backend would copy a frame of one size into a buffer of another.
      const std::string message =
          "did not stop for the format change: " + busyMessage(item.name, *item.slot);
      entry->enabled = false;
      entry->lastError = message;
      allStarted = false;
      if (error.empty()) {
        error = "'" + item.name + "' " + message;
      }
      continue;
    }
    if (!item.enabled) {
      continue;
    }
    if (!item.started) {
      entry->lastError = item.startError;
      entry->enabled = false;
      allStarted = false;
      if (error.empty()) {
        error = "'" + item.name + "': " + item.startError;
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
    std::lock_guard<std::timed_mutex> lock(mutex_);
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
  std::lock_guard<std::timed_mutex> lock(mutex_);
  return format_;
}

Engine::PreviewAccess Engine::withPreview(
    const std::function<void(PreviewOutput&)>& fn) const {
  std::shared_ptr<OutputSlot> slot;
  uint64_t generation = 0;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    for (const auto& entry : outputs_) {
      if (outputKindIsPermanent(entry.spec.kind) && entry.desc.present) {
        slot = entry.slot;
        generation = entry.desc.generation;
        break;
      }
    }
  }
  if (slot == nullptr) {
    return PreviewAccess::kNone;
  }
  // Held for the call, so an update that replaces the preview — a new scale —
  // waits for the copy rather than freeing what it is copying from.
  bool found = false;
  const bool ran = slot->call(Clock::now() + kPreviewWait, generation,
                              [&](IOutput& output) {
                                if (auto* preview = asPreview(&output)) {
                                  fn(*preview);
                                  found = true;
                                }
                              });
  if (!ran) {
    return PreviewAccess::kBusy;
  }
  return found ? PreviewAccess::kDone : PreviewAccess::kNone;
}

json::Value Engine::state() const {
  // Copy what is needed under mutex_, then ask each output through its slot
  // with the lock released. mutex_ is never held across a driver call, so the
  // wait here should never be noticeable; it is bounded anyway, because "should
  // never" is what the old code believed about Engine::state() too.
  struct Row {
    OutputSpec spec;
    std::shared_ptr<OutputSlot> slot;
    bool present = false;
    bool enabled = false;
    std::string lastError;
  };
  std::vector<Row> rows;
  VideoFormat format;
  bool audioEnabled = false;
  {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_for(kStateLockWait)) {
      std::lock_guard<std::mutex> last(lastStateMutex_);
      json::Value stale = lastState_.isObject() ? lastState_ : json::Value::object();
      stale.set("id", json::Value(id_));
      stale.set("stale", json::Value(true));
      return stale;
    }
    format = format_;
    audioEnabled = audioEnabled_;
    rows.reserve(outputs_.size());
    for (const auto& entry : outputs_) {
      rows.push_back(Row{entry.spec, entry.slot, entry.desc.present, entry.enabled,
                         entry.lastError});
    }
  }

  json::Value root = json::Value::object();
  root.set("id", json::Value(id_));
  root.set("version", json::Value(WEBLINKED_VERSION));
  root.set("running", json::Value(running_.load()));
  root.set("stale", json::Value(false));

  {
    root.set("format", json::Value(format.toString()));

    json::Value formatDetail = json::Value::object();
    formatDetail.set("width", json::Value(format.width));
    formatDetail.set("height", json::Value(format.height));
    formatDetail.set("rate_numerator", json::Value(format.rate.numerator));
    formatDetail.set("rate_denominator", json::Value(format.rate.denominator));
    formatDetail.set("interlaced", json::Value(format.interlaced));
    root.set("format_detail", formatDetail);

    // One deadline for every output together: a wedged one costs this call at
    // most the budget once, and one wedged for over a second costs nothing.
    const auto deadline = Clock::now() + kStatusBudget;
    json::Value outputs = json::Value::array();
    for (const auto& row : rows) {
      json::Value value =
          row.present ? row.slot->status(deadline) : json::Value::object();
      value.set("name", json::Value(row.spec.name));
      value.set("kind", json::Value(row.spec.kind));
      value.set("enabled", json::Value(row.enabled));
      // The spec as configured, so the settings page can populate its editor
      // without keeping a second copy of what it asked for.
      value.set("device_index", json::Value(row.spec.deviceIndex));
      value.set("options", row.spec.options);
      value.set("background", json::Value(std::string(
                                  row.spec.background.opaque ? "colour"
                                                               : "transparent")));
      // Reported even while transparent, so the editor's colour well shows what
      // turning the background on would give rather than resetting to green.
      value.set("background_colour", json::Value(row.spec.background.toHex()));
      if (!row.lastError.empty()) {
        value.set("error", json::Value(row.lastError));
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

#if defined(WEBLINKED_WITH_SCREEN)
  // Every attached display, so the settings page can offer a list of real
  // monitors instead of asking an operator to guess an index. Enumerated on
  // each state read rather than cached: a screen plugged in after start-up is
  // exactly when someone goes looking for it.
  json::Value displays = json::Value::array();
  for (const auto& display : enumerateDisplays()) {
    json::Value value = json::Value::object();
    value.set("index", json::Value(display.index));
    value.set("name", json::Value(display.name));
    value.set("width", json::Value(display.width));
    value.set("height", json::Value(display.height));
    value.set("refresh_hz", json::Value(display.refreshHz));
    value.set("primary", json::Value(display.primary));
    displays.push(value);
  }
  root.set("displays", displays);
#endif

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
    // Popups are worth surfacing rather than only logging: a page that keeps
    // trying to open windows is usually one that is about to behave oddly on
    // air, and the count is the only sign of it.
    source.set("popups", json::Value(diagnostics.popups));
    source.set("popup_policy",
               json::Value(browser_->popupPolicy() ==
                                   RenderClient::PopupPolicy::kBlock
                               ? "block"
                               : "navigate"));
    if (!diagnostics.lastPopupUrl.empty()) {
      source.set("last_popup_url", json::Value(diagnostics.lastPopupUrl));
    }
    if (!diagnostics.lastError.empty()) {
      source.set("last_error", json::Value(diagnostics.lastError));
    }
    if (!diagnostics.lastConsoleError.empty()) {
      source.set("last_console_error", json::Value(diagnostics.lastConsoleError));
    }
    root.set("source", source);
  }

  json::Value settings = json::Value::object();
  settings.set("matrix", json::Value(colourMatrixToString(matrix_.load())));
  settings.set("interactive_by_default",
               json::Value(interactiveByDefault_.load()));
  settings.set("audio_enabled", json::Value(audioEnabled));
  root.set("settings", settings);

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
  // How long since the clock thread last started a tick. A frame period or so
  // while it runs; growing without bound while state() still answers means the
  // clock is stuck inside an output's driver. This is what a supervisor should
  // watch now that /api/state no longer goes silent when that happens.
  const int64_t lastTick = lastTickNanos_.load();
  if (running_.load() && lastTick != 0) {
    pacing.set("last_tick_age_ms",
               json::Value((steadyNanos() - lastTick) / 1'000'000));
  }
  pacing.set("output_skips", json::Value(outputSkips_.load()));
  root.set("pacing", pacing);

  const auto audioStats = audio_.stats();
  json::Value audio = json::Value::object();
  audio.set("channels", json::Value(audio_.channels()));
  audio.set("sample_rate", json::Value(audio_.sampleRate()));
  audio.set("buffered_frames", json::Value(audio_.availableFrames()));
  audio.set("underruns", json::Value(audioStats.underruns));
  audio.set("overruns", json::Value(audioStats.overruns));
  root.set("audio", audio);

  {
    std::lock_guard<std::mutex> last(lastStateMutex_);
    lastState_ = root;
  }
  return root;
}

Engine::Config engineConfigFromSource(const SourceConfig& config,
                                      const std::string& cachePath) {
  Engine::Config options;
  options.id = config.id;
  options.url = config.url;
  options.format = config.format;
  options.audioEnabled = config.audioEnabled;
  options.matrix = config.matrix;
  options.pacing = config.externalPacing
                       ? BrowserSource::Pacing::kExternalBeginFrame
                       : BrowserSource::Pacing::kInternalTimer;
  options.cachePath = cachePath;
  options.popupPolicy = config.popupPolicy == "block"
                            ? RenderClient::PopupPolicy::kBlock
                            : RenderClient::PopupPolicy::kNavigateInPlace;
  options.interactiveByDefault = config.interactiveByDefault;
  options.outputs.reserve(config.outputs.size());
  for (const auto& output : config.outputs) {
    options.outputs.push_back(outputSpecFromConfig(output));
  }
  return options;
}

}  // namespace weblinked
