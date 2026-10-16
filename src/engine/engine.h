#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "browser/browser_source.h"
#include "core/audio_fifo.h"
#include "core/control_queue.h"
#include "core/frame.h"
#include "core/frame_clock.h"
#include "core/frame_ring.h"
#include "core/json.h"
#include "core/pixel_convert.h"
#include "core/video_format.h"
#include "engine/input_event.h"
#include "outputs/output.h"
#include "outputs/output_slot.h"

namespace weblinked {

class PreviewOutput;

/// Owns the pipeline: one browser source, one clock, N outputs.
///
/// The clock thread is the only thing that touches an output's submit(). Control
/// requests arrive on the HTTP or OSC thread and either mutate guarded state or
/// post to CEF's UI thread.
///
/// Every call into an output goes through its OutputSlot, and `mutex_` is never
/// held while one is made: a driver that deadlocks inside a call (issue #12)
/// then holds that output's gate and nothing else. Every wait on a gate has a
/// deadline, so a wedged output costs its own frames and an error on the
/// requests that need it — never the clock, the other outputs or the control
/// API.
class Engine {
 public:
  struct Config {
    /// Stable handle the control API and OSC address this pipeline by. Defaults
    /// to the name the single-source build has always reported, so a client that
    /// predates the source manager still finds "main" where it left it.
    std::string id = "main";
    std::string url = "about:blank";
    VideoFormat format;
    std::vector<OutputSpec> outputs;
    bool audioEnabled = true;
    ColourMatrix matrix = ColourMatrix::kAuto;
    BrowserSource::Pacing pacing = BrowserSource::Pacing::kExternalBeginFrame;
    std::string cachePath;
    RenderClient::PopupPolicy popupPolicy =
        RenderClient::PopupPolicy::kNavigateInPlace;
    /// Whether the control page arms its preview for input on load. On by
    /// default: the preview is the only way to reach a page that wants a click
    /// before it will show anything, and an operator who has to find a toggle
    /// first usually concludes the preview is broken. Reported in state() so
    /// the page does not have to guess.
    bool interactiveByDefault = true;
  };

  Engine();
  ~Engine();

  /// This pipeline's handle. Fixed at start() and never changes: the manager
  /// keys its map on it, and a rename underneath that map would strand the
  /// engine. Renaming is done by the manager, not here.
  const std::string& id() const { return id_; }

  /// Creates the browser and opens every configured output. An output that fails
  /// to open is reported but does not stop the others: on site, one missing card
  /// must not take the NDI feed down with it.
  bool start(const Config& config, std::string& error);
  void stop();

  /// The half of stop() that does not touch the browser: parks the clock for
  /// good and closes every output, which withdraws NDI senders and releases
  /// cards. Safe to call on the UI thread while the message loop is still
  /// running — unlike closing the offscreen browser, which must wait until the
  /// loop has returned (see beginShutdown() in main.cpp). stop() calls it too,
  /// so calling both is fine.
  void halt();

  // --- control surface, all thread-safe -------------------------------------

  void setUrl(const std::string& url);
  void reload(bool ignoreCache);
  void runScript(const std::string& script);
  void setAudioMuted(bool muted);
  bool audioMuted() const { return audioMuted_.load(); }

  /// Swaps who drives frames without restarting anything. Only meaningful
  /// between ticks, which is why it is a plain store the clock thread reads.
  void setPacing(BrowserSource::Pacing pacing);

  void setPopupPolicy(RenderClient::PopupPolicy policy);

  /// Whether the control page arms its preview for input on load. Held here
  /// rather than in the page so it survives a reload and is the same for every
  /// operator who opens the page.
  void setInteractiveByDefault(bool interactive) {
    interactiveByDefault_.store(interactive);
  }
  bool interactiveByDefault() const { return interactiveByDefault_.load(); }

  /// The colour matrix used for every RGB to Y'CbCr conversion. Takes effect on
  /// the next tick; no output has to restart.
  void setMatrix(ColourMatrix matrix);
  ColourMatrix matrix() const;

  /// The outputs as configured, in order — what the settings page edits, as
  /// opposed to state() which reports what they are doing.
  std::vector<OutputSpec> outputSpecs() const;

  /// The live pipeline as a saveable configuration.
  SourceConfig configuration() const;

  /// Makes the live pipeline match `config`.
  ///
  /// Reconciles rather than rebuilds: an output that is unchanged is left
  /// running, so saving a settings change to the NDI name does not interrupt
  /// the SDI feed beside it. Returns false with `error` naming the first thing
  /// that could not be applied — everything else still is, because a settings
  /// file that half-applies and reports honestly beats one that refuses
  /// wholesale because a card is missing.
  bool applyConfiguration(const SourceConfig& config, std::string& error);

  /// Enables or disables an output by name. Disabling stops the device and frees
  /// it for another application; it does not remove it from the list.
  bool setOutputEnabled(const std::string& name, bool enabled, std::string& error);

  bool addOutput(const OutputSpec& spec, std::string& error);

  /// Removes an output by name. Refuses for a permanent one — see
  /// `outputKindIsPermanent` — so the preview survives both an operator's
  /// Remove and a settings file that has lost it. `error` is optional because
  /// `applyConfiguration` removes in a loop where a refusal is the intended
  /// outcome and there is nobody to tell.
  bool removeOutput(const std::string& name, std::string* error = nullptr);

  /// Changes what one output composites the page over, without restarting it.
  ///
  /// Nothing in a backend acts on the background — the engine composites before
  /// the frame is handed over — so this is a field assignment under the same
  /// lock the clock thread reads the specs under, and takes effect on the next
  /// tick. An operator nudging a green while a keyer watches must not cost a
  /// dropped frame, which reopening the device would.
  bool setOutputBackground(const std::string& name,
                           const OutputBackground& background,
                           std::string& error);

  /// Replaces the output called `name` with `spec`, keeping its position in the
  /// list.
  ///
  /// The settings page needs this rather than a remove followed by an add: if
  /// the add failed — a card already claimed, an NDI name in use — the operator
  /// would be left with no output at all, having asked only to rename one. On
  /// failure the previous output is restarted and `error` explains why.
  bool updateOutput(const std::string& name, const OutputSpec& spec,
                    std::string& error);

  /// Changes raster or rate. Every output is stopped and reopened, because no
  /// SDI device can change mode while running.
  bool setFormat(const VideoFormat& format, std::string& error);

  /// Forwards a pointer or keyboard event to the page.
  ///
  /// Normalised coordinates are scaled to the current raster here, under the
  /// same lock that guards a format change — so a click cannot be scaled against
  /// a raster that has just been replaced.
  void sendInput(const InputEvent& event);

  VideoFormat format() const;

  /// The whole state tree, as served by GET /api/state.
  ///
  /// Never waits for long. An output whose gate is held — its driver busy in a
  /// call — reports the last status that completed, with "stale": true and how
  /// long the call has been running. If even the engine's own mutex cannot be
  /// had promptly, the whole of the last report comes back, marked stale.
  json::Value state() const;

  /// Runs `fn` against the preview output, holding its slot so that it cannot
  /// be replaced or destroyed during the call. kBusy if the slot could not be
  /// had in time, which is not the same as there being no preview.
  enum class PreviewAccess { kDone, kNone, kBusy };
  PreviewAccess withPreview(const std::function<void(PreviewOutput&)>& fn) const;

 private:
  void clockLoop();

  /// The second half of halt(): stops and destroys every output through its
  /// slot. Safe with the clock thread still running, or stuck.
  void closeOutputs();

  /// Parks the clock thread and returns once it has acknowledged, or after a
  /// second if it has not — false then, and the caller must not touch
  /// clock-thread state: the thread is somewhere else, most likely inside an
  /// output's driver. Callers must pair this with resumeClock(); the RAII guard
  /// below does that.
  bool pauseClock();
  void resumeClock();

  /// Scoped pause, so an early return cannot leave the clock parked forever.
  class ClockPause {
   public:
    explicit ClockPause(Engine* engine)
        : engine_(engine), parked_(engine_->pauseClock()) {}
    ~ClockPause() { engine_->resumeClock(); }
    ClockPause(const ClockPause&) = delete;
    ClockPause& operator=(const ClockPause&) = delete;

    bool parked() const { return parked_; }

   private:
    Engine* engine_;
    bool parked_;
  };
  /// Sample-frames of audio belonging to `tick`.
  ///
  /// Computed as the difference of two exact positions rather than a constant,
  /// because at 59.94 the correct answer alternates between 800 and 801 and a
  /// constant 800 loses a sample every five frames — about 40 ms of drift a
  /// minute, which is an audible lip-sync error inside a couple of minutes.
  int audioFramesForTick(int64_t tick) const;

  /// Returns `source` converted into `target`, reusing per-format scratch frames.
  const VideoFrame* frameInFormat(const VideoFramePtr& source, PixelFormat target);

  /// Returns `source` with its alpha un-premultiplied, for keyed outputs.
  /// Null if the source is the wrong raster, exactly as frameInFormat.
  const VideoFrame* unpremultiplied(const VideoFramePtr& source);

  /// One background colour composited under the page, in whichever pixel
  /// formats the outputs asking for that colour want.
  ///
  /// Keyed by colour rather than held per output so that four SDI feeds on the
  /// same green cost one composite between them, and computed at most once per
  /// tick per format — `bgraTick` and `uyvyTick` are what make the second
  /// output asking a lookup rather than a second pass over eight megabytes.
  struct BackgroundVariant {
    uint32_t colour = 0;  ///< 0x00RRGGBB
    VideoFramePtr bgra;   ///< the composite, opaque
    VideoFramePtr uyvy;   ///< converted from the composite, on demand
    int64_t bgraTick = -1;
    int64_t uyvyTick = -1;
  };

  /// `source` over `colour`, as opaque BGRA. Null on a raster mismatch, exactly
  /// as frameInFormat.
  const VideoFrame* compositedBgra(const VideoFramePtr& source, uint32_t colour,
                                   int64_t tick);
  /// The same composite, converted to UYVY.
  const VideoFrame* compositedUyvy(const VideoFramePtr& source, uint32_t colour,
                                   int64_t tick);
  BackgroundVariant& backgroundVariant(uint32_t colour);

  struct Entry {
    OutputSpec spec;
    /// Never null. Empty (desc.present false) for an output that could not be
    /// created. Shared so that the clock thread and a state report can hold it
    /// across a call made without mutex_.
    std::shared_ptr<OutputSlot> slot;
    /// The slot's description as of the last control path to have it. The
    /// clock plans each tick from this, under mutex_, without calling into the
    /// output; the generation inside it is how the call finds out the plan is
    /// stale.
    OutputSlot::Description desc;
    bool enabled = true;
    std::string lastError;
  };

  /// Under mutex_. Null if there is no output called `name`.
  Entry* findEntry(const std::string& name);

  /// The message a control request gets when an output's gate cannot be had.
  static std::string busyMessage(const std::string& name, const OutputSlot& slot);

  /// Written once by start() before any other thread can see this engine, so it
  /// needs no guarding.
  std::string id_ = "main";

  /// Guards outputs_, format_ and audioEnabled_. Never held across a call into
  /// an output, a browser or anything else that can block — only while copying
  /// in or out — so a wait for it is short. Timed only so that state() can
  /// prove it.
  mutable std::timed_mutex mutex_;
  std::vector<Entry> outputs_;
  VideoFormat format_;

  /// Serialises the control paths that change the output list or open and
  /// close devices: add, remove, update, enable and format changes. Its mutex
  /// (controlMutex()) is held across their driver calls — which is exactly why
  /// neither the clock thread nor state() ever takes it. A change waits up to
  /// 30 s for the one in front, but gives up at once if four are already
  /// waiting or the one in front is stuck in a driver (see ControlQueue): each
  /// waiter is an HTTP thread, and the server has 64.
  ControlQueue controlQueue_{std::chrono::seconds(30), 4};

  /// Takes the control queue for a change, or fills `error` with why not.
  bool enterControl(std::unique_lock<std::timed_mutex>& control, std::string& error);

  /// The output the change holding the queue is calling into, so a waiter can
  /// tell a slow change from a stuck one. Set and cleared by ControlCall.
  mutable std::mutex controlCallMutex_;
  std::shared_ptr<OutputSlot> controlCallSlot_;
  std::string controlCallName_;

  /// Marks a control path's call into one output for as long as it lasts.
  class ControlCall {
   public:
    ControlCall(Engine* engine, std::shared_ptr<OutputSlot> slot, const std::string& name)
        : engine_(engine) {
      std::lock_guard<std::mutex> lock(engine_->controlCallMutex_);
      engine_->controlCallSlot_ = std::move(slot);
      engine_->controlCallName_ = name;
    }
    ~ControlCall() {
      std::lock_guard<std::mutex> lock(engine_->controlCallMutex_);
      engine_->controlCallSlot_.reset();
      engine_->controlCallName_.clear();
    }
    ControlCall(const ControlCall&) = delete;
    ControlCall& operator=(const ControlCall&) = delete;

   private:
    Engine* engine_;
  };

  /// The last complete state(), for when mutex_ cannot be had in time.
  mutable std::mutex lastStateMutex_;
  mutable json::Value lastState_;
  /// Atomic rather than mutex-guarded because the clock thread reads it inside
  /// frameInFormat(), outside mutex_, on every converted frame — and the
  /// settings page can now change it while that is happening.
  std::atomic<ColourMatrix> matrix_{ColourMatrix::kAuto};
  bool audioEnabled_ = true;

  LatestFrameSlot slot_;
  AudioFifo audio_;
  std::unique_ptr<BrowserSource> browser_;
  std::unique_ptr<FrameClock> clock_;

  std::thread clockThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> audioMuted_{false};
  std::atomic<bool> interactiveByDefault_{true};

  /// Parks the clock thread so another thread can safely rebuild the
  /// clock-thread-owned state below.
  ///
  /// Needed because a raster change has to replace the frame pools, the black
  /// frame and the scratch frame — all of which the clock thread reads *outside*
  /// the output mutex, at the top of its loop, before it knows whether it needs
  /// them. Reassigning a shared_ptr from the HTTP thread while the clock thread
  /// copies it is a torn refcount and a dead thread, and a thread that dies
  /// holding mutex_ takes the whole control API down with it. Asking the clock
  /// thread to stand still for a moment is far simpler than making every one of
  /// those fields individually safe.
  std::mutex pauseMutex_;
  std::condition_variable pauseCv_;
  bool pauseRequested_ = false;
  bool paused_ = false;

  /// Set by the clock thread as it leaves, so halt() can wait for it with a
  /// deadline before falling back to an unbounded join.
  std::mutex exitMutex_;
  std::condition_variable exitCv_;
  bool clockExited_ = false;

  /// Bumped on every format change so the clock thread knows to forget the frame
  /// it was holding, which belongs to the previous raster.
  std::atomic<int64_t> formatEpoch_{0};

  // Clock-thread-only state. Only ever touched by clockLoop, or by another
  // thread while the clock thread is parked — see pauseMutex_ above.

  /// One tick's outputs, copied out of outputs_ under mutex_ and called after
  /// it is released. Cleared at the end of every tick so a removed output's
  /// slot is not kept alive by the clock.
  struct Target {
    std::shared_ptr<OutputSlot> slot;
    OutputSlot::Description desc;
    OutputBackground background;
  };
  std::vector<Target> targets_;
  FramePoolPtr uyvyPool_;
  FramePoolPtr blackPool_;
  FramePoolPtr straightPool_;
  FramePoolPtr backgroundPool_;
  VideoFramePtr uyvyScratch_;
  VideoFramePtr straightScratch_;
  VideoFramePtr blackFrame_;
  /// One entry per background colour any enabled output is currently asking
  /// for. Pruned every tick rather than allowed to grow: an operator dragging a
  /// colour picker would otherwise leave a raster-sized pair of buffers behind
  /// for every shade they passed through.
  std::vector<BackgroundVariant> backgrounds_;
  std::vector<uint32_t> backgroundKeys_;
  std::vector<float> audioInterleaved_;
  std::vector<std::vector<float>> audioPlanes_;
  std::vector<float*> audioPlanePointers_;

  std::atomic<int64_t> ticks_{0};
  std::atomic<int64_t> repeatedFrames_{0};
  /// steady_clock nanoseconds at which the clock thread last started a tick. A
  /// state report that keeps answering while this stops moving is the sign of
  /// a clock thread stuck inside a driver — which /api/state going silent used
  /// to be, before it could no longer go silent.
  std::atomic<int64_t> lastTickNanos_{0};
  /// Submits skipped because an output's gate could not be had in time.
  std::atomic<int64_t> outputSkips_{0};
};

/// Turns the pure-data description of a pipeline into the engine's own config.
///
/// The inverse of Engine::configuration(). It lives here rather than in `core`
/// because the result names CEF types; SourceConfig deliberately does not, so
/// that a configuration file can still be parsed and validated in a build with
/// no browser in it.
///
/// `cachePath` is passed in rather than carried in the file: every source in one
/// process shares Chromium's cache directory, and letting a config file name a
/// different one per source would mean several Chromium profiles in a single
/// process, which CEF does not allow.
Engine::Config engineConfigFromSource(const SourceConfig& config,
                                      const std::string& cachePath);

}  // namespace weblinked
