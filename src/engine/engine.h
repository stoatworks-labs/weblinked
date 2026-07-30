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
    std::string url = "about:blank";
    VideoFormat format;
    std::vector<OutputSpec> outputs;
    bool audioEnabled = true;
    ColourMatrix matrix = ColourMatrix::kAuto;
    BrowserSource::Pacing pacing = BrowserSource::Pacing::kExternalBeginFrame;
    std::string cachePath;
  };

  Engine();
  ~Engine();

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

  /// Enables or disables an output by name. Disabling stops the device and frees
  /// it for another application; it does not remove it from the list.
  bool setOutputEnabled(const std::string& name, bool enabled, std::string& error);

  bool addOutput(const OutputSpec& spec, std::string& error);
  bool removeOutput(const std::string& name);

  /// Changes raster or rate. Every output is stopped and reopened, because no
  /// SDI device can change mode while running.
  bool setFormat(const VideoFormat& format, std::string& error);

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

  struct Entry {
    OutputSpec spec;
    std::unique_ptr<IOutput> output;
    bool enabled = true;
    std::string lastError;
  };

  mutable std::mutex mutex_;   ///< guards outputs_ and format_
  std::vector<Entry> outputs_;
  VideoFormat format_;
  ColourMatrix matrix_ = ColourMatrix::kAuto;
  bool audioEnabled_ = true;

  LatestFrameSlot slot_;
  AudioFifo audio_;
  std::unique_ptr<BrowserSource> browser_;
  std::unique_ptr<FrameClock> clock_;

  std::thread clockThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> audioMuted_{false};

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
  VideoFramePtr uyvyScratch_;
  VideoFramePtr blackFrame_;
  std::vector<float> audioInterleaved_;
  std::vector<std::vector<float>> audioPlanes_;
  std::vector<float*> audioPlanePointers_;

  std::atomic<int64_t> ticks_{0};
  std::atomic<int64_t> repeatedFrames_{0};
};

}  // namespace weblinked
