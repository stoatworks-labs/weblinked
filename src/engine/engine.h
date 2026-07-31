#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "browser/browser_source.h"
#include "core/audio_fifo.h"
#include "core/frame.h"
#include "core/frame_clock.h"
#include "core/frame_ring.h"
#include "core/json.h"
#include "core/pixel_convert.h"
#include "core/video_format.h"
#include "engine/input_event.h"
#include "outputs/output.h"

namespace weblinked {

class PreviewOutput;

/// Owns the pipeline: one browser source, one clock, N outputs.
///
/// The clock thread is the only thing that touches an output's submit(). Control
/// requests arrive on the HTTP or OSC thread and either mutate guarded state or
/// post to CEF's UI thread; none of them reach into an output directly.
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
  bool removeOutput(const std::string& name);

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
  json::Value state() const;

  PreviewOutput* preview() const;

 private:
  void clockLoop();

  /// Parks the clock thread and returns once it has acknowledged. Callers must
  /// pair this with resumeClock(); the RAII guard below does that.
  void pauseClock();
  void resumeClock();

  /// Scoped pause, so an early return cannot leave the clock parked forever.
  class ClockPause {
   public:
    explicit ClockPause(Engine* engine) : engine_(engine) { engine_->pauseClock(); }
    ~ClockPause() { engine_->resumeClock(); }
    ClockPause(const ClockPause&) = delete;
    ClockPause& operator=(const ClockPause&) = delete;

   private:
    Engine* engine_;
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

  struct Entry {
    OutputSpec spec;
    std::unique_ptr<IOutput> output;
    bool enabled = true;
    std::string lastError;
  };

  /// Written once by start() before any other thread can see this engine, so it
  /// needs no guarding.
  std::string id_ = "main";

  mutable std::mutex mutex_;   ///< guards outputs_ and format_
  std::vector<Entry> outputs_;
  VideoFormat format_;
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

  /// Bumped on every format change so the clock thread knows to forget the frame
  /// it was holding, which belongs to the previous raster.
  std::atomic<int64_t> formatEpoch_{0};

  // Clock-thread-only state. Only ever touched by clockLoop, or by another
  // thread while the clock thread is parked — see pauseMutex_ above.
  FramePoolPtr uyvyPool_;
  FramePoolPtr blackPool_;
  FramePoolPtr straightPool_;
  VideoFramePtr uyvyScratch_;
  VideoFramePtr straightScratch_;
  VideoFramePtr blackFrame_;
  std::vector<float> audioInterleaved_;
  std::vector<std::vector<float>> audioPlanes_;
  std::vector<float*> audioPlanePointers_;

  std::atomic<int64_t> ticks_{0};
  std::atomic<int64_t> repeatedFrames_{0};
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
