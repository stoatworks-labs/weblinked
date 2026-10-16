// dl_fake_card — drives the real DeckLink backend against a simulated card.
//
// Why this exists: issues #12 and #13 were found on a real rig, and no DeckLink
// card was attached when they were fixed. Rather than claim the fix works
// because it compiles, this links the *actual* src/outputs/decklink_output.cpp
// against a stand-in for the SDK — CreateDeckLinkIteratorInstance and the
// interfaces behind it — and drives it the way the engine does, one call at a
// time, at the frame rate.
//
// The stand-in plays out scheduled frames on its own "hardware" thread by the
// SDK's documented rules (the least late due frame is shown; one overtaken by
// a less late one is dropped; one shown after its time is late), delivers
// completions on a separate thread and releases its own reference after each,
// as the SDK does, and blocks DisableVideoOutput until every scheduled frame
// has been completed or flushed. It also counts what #12 was about: frames
// whose memory comes from the SDK's pool (CreateVideoFrame) created or
// destroyed while playback runs, and destroyed on the completion thread — the
// path that deadlocked inside Desktop Video 16.4 — against frames wrapping the
// application's own memory (CreateVideoFrameWithBuffer), which leave the pool
// alone. While a frame wrapping an application buffer is with the card, the
// card holds a read access open on that buffer, as a transfer would; the
// output counts any write that starts during one.
//
// What it therefore proves and does not:
//   * proves — the ring hands out, recycles and finally releases every frame
//     exactly once; no frame is written while the card has it; on SDK 14.3 and
//     later no frame comes from the SDK's pool at all, and the memory is made
//     once and kept across stop/start; after a stall longer than the queue
//     the schedule re-anchors once and later frames are on time; stop/start
//     cycles leave nothing behind.
//   * does NOT prove — anything about real hardware or the real SDK: its
//     allocator, its threads, its timing, what it does with application
//     buffers, or whether its playhead behaves like this one. That waits for a
//     card. See docs/04-verification.md section 40.
//
// Build (SDK 14.3 or later — the frame here speaks IDeckLinkVideoBuffer):
//   SDK=~/sdk/decklink-16.0/Mac/include
//   clang++ -std=c++20 -O1 -I"$SDK" -Isrc -DWEBLINKED_WITH_DECKLINK=1 \
//     tools/dl_fake_card.cpp src/outputs/decklink_output.cpp \
//     src/outputs/output.cpp src/outputs/preview_output.cpp \
//     build/libweblinked_core.a -framework CoreFoundation \
//     -framework SystemConfiguration -o dl_fake_card
//   ./dl_fake_card

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DeckLinkAPI.h"
#include "DeckLinkAPIVersion.h"
#include "core/frame.h"
#include "core/video_format.h"
#include "outputs/output.h"

#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0e030000
#error "dl_fake_card models SDK 14.3 or later (IDeckLinkVideoBuffer)"
#endif

namespace {

using Clock = std::chrono::steady_clock;

bool sameIid(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

/// Everything the harness asserts on.
struct Ledger {
  std::atomic<int64_t> created{0};
  std::atomic<int64_t> destroyed{0};
  std::atomic<int64_t> poolFrames{0};  ///< made by CreateVideoFrame: SDK memory
  std::atomic<int64_t> appFrames{0};   ///< made by CreateVideoFrameWithBuffer
  std::atomic<int64_t> createdWhilePlaying{0};
  std::atomic<int64_t> destroyedWhilePlaying{0};
  std::atomic<int64_t> destroyedOnCompletionThread{0};
  std::atomic<int64_t> writesWhileScheduled{0};
  std::atomic<int64_t> slotsScheduledTwice{0};
  std::atomic<int64_t> onTime{0};
  std::atomic<int64_t> late{0};
  std::atomic<int64_t> dropped{0};
  std::atomic<int64_t> flushed{0};
  std::atomic<bool> playing{false};
  std::thread::id completionThread;
  std::mutex buffersMutex;
  std::set<IDeckLinkVideoBuffer*> buffers;  ///< distinct application buffers seen
};
Ledger gLedger;

class FakeFrame final : public IDeckLinkMutableVideoFrame, public IDeckLinkVideoBuffer {
 public:
  /// `external` null: the SDK's own memory, as CreateVideoFrame gives.
  /// Otherwise the application's buffer, held for the frame's life.
  FakeFrame(long width, long height, long rowBytes, BMDPixelFormat format,
            IDeckLinkVideoBuffer* external)
      : width_(width), height_(height), rowBytes_(rowBytes), format_(format),
        external_(external) {
    ++gLedger.created;
    if (external_ != nullptr) {
      external_->AddRef();
      ++gLedger.appFrames;
    } else {
      bytes_.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(height));
      ++gLedger.poolFrames;
      if (gLedger.playing.load()) {
        ++gLedger.createdWhilePlaying;
      }
    }
  }

  IDeckLinkVideoBuffer* external() const { return external_; }

  std::atomic<bool> scheduled{false};
  BMDTimeValue displayTime = 0;

  // IUnknown — one implementation for both bases.
  HRESULT QueryInterface(REFIID iid, LPVOID* out) override {
    if (sameIid(iid, IID_IDeckLinkVideoBuffer)) {
      if (external_ != nullptr) {
        return external_->QueryInterface(iid, out);
      }
      *out = static_cast<IDeckLinkVideoBuffer*>(this);
      AddRef();
      return S_OK;
    }
    if (sameIid(iid, IID_IDeckLinkMutableVideoFrame) ||
        sameIid(iid, IID_IDeckLinkVideoFrame)) {
      *out = static_cast<IDeckLinkMutableVideoFrame*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      ++gLedger.destroyed;
      // Only a frame of the SDK's own memory reaches its allocator when it
      // goes; one wrapping an application buffer releases that and nothing else.
      if (external_ == nullptr) {
        if (gLedger.playing.load()) {
          ++gLedger.destroyedWhilePlaying;
        }
        if (std::this_thread::get_id() == gLedger.completionThread) {
          ++gLedger.destroyedOnCompletionThread;
        }
      } else {
        external_->Release();
      }
      delete this;
    }
    return left;
  }

  // IDeckLinkVideoFrame
  long GetWidth() override { return width_; }
  long GetHeight() override { return height_; }
  long GetRowBytes() override { return rowBytes_; }
  BMDPixelFormat GetPixelFormat() override { return format_; }
  BMDFrameFlags GetFlags() override { return bmdFrameFlagDefault; }
  HRESULT GetTimecode(BMDTimecodeFormat, IDeckLinkTimecode**) override { return E_NOTIMPL; }
  HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary**) override { return E_NOTIMPL; }

  // IDeckLinkMutableVideoFrame
  HRESULT SetFlags(BMDFrameFlags) override { return S_OK; }
  HRESULT SetTimecode(BMDTimecodeFormat, IDeckLinkTimecode*) override { return E_NOTIMPL; }
  HRESULT SetTimecodeFromComponents(BMDTimecodeFormat, uint8_t, uint8_t, uint8_t, uint8_t,
                                    BMDTimecodeFlags) override {
    return E_NOTIMPL;
  }
  HRESULT SetAncillaryData(IDeckLinkVideoFrameAncillary*) override { return E_NOTIMPL; }
  HRESULT SetTimecodeUserBits(BMDTimecodeFormat, BMDTimecodeUserBits) override {
    return E_NOTIMPL;
  }
  HRESULT SetInterfaceProvider(REFIID, IUnknown*) override { return E_NOTIMPL; }

  // IDeckLinkVideoBuffer
  HRESULT GetBytes(void** buffer) override {
    *buffer = bytes_.data();
    return S_OK;
  }
  HRESULT GetSize(uint64_t* size) override {
    *size = bytes_.size();
    return S_OK;
  }
  HRESULT StartAccess(BMDBufferAccessFlags flags) override {
    if ((flags & bmdBufferAccessWrite) != 0 && scheduled.load()) {
      ++gLedger.writesWhileScheduled;
    }
    return S_OK;
  }
  HRESULT EndAccess(BMDBufferAccessFlags) override { return S_OK; }

 private:
  ~FakeFrame() override = default;
  std::atomic<ULONG> refs_{1};
  long width_, height_, rowBytes_;
  BMDPixelFormat format_;
  IDeckLinkVideoBuffer* external_ = nullptr;
  std::vector<uint8_t> bytes_;
};

class FakeMode final : public IDeckLinkDisplayMode {
 public:
  FakeMode(BMDDisplayMode mode, long w, long h, BMDTimeValue duration, BMDTimeScale scale)
      : mode_(mode), w_(w), h_(h), duration_(duration), scale_(scale) {}
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT GetName(CFStringRef* name) override {
    *name = nullptr;
    return E_NOTIMPL;
  }
  BMDDisplayMode GetDisplayMode() override { return mode_; }
  long GetWidth() override { return w_; }
  long GetHeight() override { return h_; }
  HRESULT GetFrameRate(BMDTimeValue* duration, BMDTimeScale* scale) override {
    *duration = duration_;
    *scale = scale_;
    return S_OK;
  }
  BMDFieldDominance GetFieldDominance() override { return bmdProgressiveFrame; }
  BMDDisplayModeFlags GetFlags() override { return 0; }

 private:
  ~FakeMode() override = default;
  std::atomic<ULONG> refs_{1};
  BMDDisplayMode mode_;
  long w_, h_;
  BMDTimeValue duration_;
  BMDTimeScale scale_;
};

struct ModeInfo {
  BMDDisplayMode mode;
  long w, h;
  BMDTimeValue duration;
  BMDTimeScale scale;
};
const ModeInfo kModes[] = {
    {bmdModeHD1080p25, 1920, 1080, 1000, 25000},
    {bmdModeHD1080p50, 1920, 1080, 1000, 50000},
    {bmdModeHD1080p5994, 1920, 1080, 1001, 60000},
};

class FakeModeIterator final : public IDeckLinkDisplayModeIterator {
 public:
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT Next(IDeckLinkDisplayMode** mode) override {
    if (index_ >= std::size(kModes)) {
      *mode = nullptr;
      return S_FALSE;
    }
    const auto& m = kModes[index_++];
    *mode = new FakeMode(m.mode, m.w, m.h, m.duration, m.scale);
    return S_OK;
  }

 private:
  ~FakeModeIterator() override = default;
  std::atomic<ULONG> refs_{1};
  size_t index_ = 0;
};

class FakeKeyer final : public IDeckLinkKeyer {
 public:
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override { return --refs_; }  // owned by the device
  HRESULT Enable(bool) override { return S_OK; }
  HRESULT SetLevel(uint8_t) override { return S_OK; }
  HRESULT RampUp(uint32_t) override { return S_OK; }
  HRESULT RampDown(uint32_t) override { return S_OK; }
  HRESULT Disable() override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
};

/// The card. Scheduled playback by the SDK's documented rules, a playhead that
/// moves one frame per frame period of real time, and a completion thread.
class FakeOutput final : public IDeckLinkOutput {
 public:
  FakeOutput() {
    completionThread_ = std::thread([this] { completionLoop(); });
    gLedger.completionThread = completionThread_.get_id();
  }
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    signal_.notify_all();
    completionThread_.join();
  }

  /// A SIGSTOP of the process stops the SDK's completion thread too, while
  /// the hardware carries on. This is that.
  void pauseCompletions(bool paused) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completionsPaused_ = paused;
    }
    signal_.notify_all();
  }

  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override { return --refs_; }  // owned by the harness

  HRESULT DoesSupportVideoMode(BMDVideoConnection, BMDDisplayMode mode, BMDPixelFormat,
                               BMDVideoOutputConversionMode, BMDSupportedVideoModeFlags,
                               BMDDisplayMode* actual, bool* supported) override {
    if (actual != nullptr) *actual = mode;
    *supported = true;
    return S_OK;
  }
  HRESULT GetDisplayMode(BMDDisplayMode, IDeckLinkDisplayMode**) override { return E_NOTIMPL; }
  HRESULT GetDisplayModeIterator(IDeckLinkDisplayModeIterator** iterator) override {
    *iterator = new FakeModeIterator();
    return S_OK;
  }
  HRESULT SetScreenPreviewCallback(IDeckLinkScreenPreviewCallback*) override { return S_OK; }

  HRESULT EnableVideoOutput(BMDDisplayMode mode, BMDVideoOutputFlags) override {
    for (const auto& m : kModes) {
      if (m.mode == mode) {
        mode_ = m;
        videoEnabled_ = true;
        return S_OK;
      }
    }
    return E_FAIL;
  }

  HRESULT DisableVideoOutput() override {
    // "DisableVideoOutput will block until all scheduled frames are completed
    // or flushed" — the SDK's SignalGenerator sample, and what stop() relies on.
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto& [time, frame] : queue_) {
      pending_.push_back({frame, bmdOutputFrameFlushed});
    }
    queue_.clear();
    if (onAir_ != nullptr) {
      pending_.push_back({onAir_, onAirResult_});
      onAir_ = nullptr;
    }
    signal_.notify_all();
    signal_.wait(lock, [&] { return pending_.empty() && !delivering_; });
    videoEnabled_ = false;
    return S_OK;
  }

  HRESULT CreateVideoFrame(int32_t width, int32_t height, int32_t rowBytes,
                           BMDPixelFormat format, BMDFrameFlags,
                           IDeckLinkMutableVideoFrame** out) override {
    *out = new FakeFrame(width, height, rowBytes, format, nullptr);
    return S_OK;
  }
  HRESULT CreateVideoFrameWithBuffer(int32_t width, int32_t height, int32_t rowBytes,
                                     BMDPixelFormat format, BMDFrameFlags,
                                     IDeckLinkVideoBuffer* buffer,
                                     IDeckLinkMutableVideoFrame** out) override {
    // The manual's one requirement on the buffer: at least
    // RowBytesForPixelFormat x height bytes.
    int32_t minimum = 0;
    RowBytesForPixelFormat(format, width, &minimum);
    uint64_t size = 0;
    if (buffer == nullptr || buffer->GetSize(&size) != S_OK || rowBytes < minimum ||
        size < static_cast<uint64_t>(rowBytes) * static_cast<uint64_t>(height)) {
      return E_INVALIDARG;
    }
    {
      std::lock_guard<std::mutex> lock(gLedger.buffersMutex);
      gLedger.buffers.insert(buffer);
    }
    *out = new FakeFrame(width, height, rowBytes, format, buffer);
    return S_OK;
  }
  HRESULT RowBytesForPixelFormat(BMDPixelFormat format, int32_t width,
                                 int32_t* rowBytes) override {
    *rowBytes = format == bmdFormat8BitYUV ? width * 2 : width * 4;
    return S_OK;
  }
  HRESULT CreateAncillaryData(BMDPixelFormat, IDeckLinkVideoFrameAncillary**) override {
    return E_NOTIMPL;
  }
  HRESULT DisplayVideoFrameSync(IDeckLinkVideoFrame*) override { return E_NOTIMPL; }

  HRESULT ScheduleVideoFrame(IDeckLinkVideoFrame* frame, BMDTimeValue displayTime,
                             BMDTimeValue duration, BMDTimeScale scale) override {
    if (!videoEnabled_) return E_ACCESSDENIED;
    // Into the mode's own units, which is what the playhead counts in.
    const BMDTimeValue time = displayTime * mode_.scale / scale;
    (void)duration;
    auto* fake = static_cast<FakeFrame*>(static_cast<IDeckLinkMutableVideoFrame*>(
        static_cast<IDeckLinkVideoFrame*>(frame)));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!usedSlots_.insert(time).second) {
      ++gLedger.slotsScheduledTwice;
    }
    fake->AddRef();  // the card's own reference, released after completion
    fake->scheduled.store(true);
    if (fake->external() != nullptr) {
      // A transfer reads the application's buffer from now until completion.
      fake->external()->StartAccess(bmdBufferAccessRead);
    }
    fake->displayTime = time;
    queue_.emplace(time, fake);
    return S_OK;
  }

  HRESULT SetScheduledFrameCompletionCallback(IDeckLinkVideoOutputCallback* callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback != nullptr) callback->AddRef();
    if (callback_ != nullptr) callback_->Release();
    callback_ = callback;
    return S_OK;
  }
  HRESULT GetBufferedVideoFrameCount(uint32_t* count) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *count = static_cast<uint32_t>(queue_.size());
    return S_OK;
  }

  HRESULT EnableAudioOutput(BMDAudioSampleRate, BMDAudioSampleType, uint32_t,
                            BMDAudioOutputStreamType) override {
    return S_OK;
  }
  HRESULT DisableAudioOutput() override { return S_OK; }
  HRESULT WriteAudioSamplesSync(void*, uint32_t, uint32_t*) override { return E_NOTIMPL; }
  HRESULT BeginAudioPreroll() override { return S_OK; }
  HRESULT EndAudioPreroll() override { return S_OK; }
  HRESULT ScheduleAudioSamples(void*, uint32_t count, BMDTimeValue, BMDTimeScale,
                               uint32_t* written) override {
    if (written != nullptr) *written = count;
    return S_OK;
  }
  HRESULT GetBufferedAudioSampleFrameCount(uint32_t* count) override {
    *count = 0;
    return S_OK;
  }
  HRESULT FlushBufferedAudioSamples() override { return S_OK; }
  HRESULT SetAudioCallback(IDeckLinkAudioOutputCallback*) override { return S_OK; }

  HRESULT StartScheduledPlayback(BMDTimeValue, BMDTimeScale, double) override {
    std::lock_guard<std::mutex> lock(mutex_);
    playhead_ = 0;
    hardwareStop_ = false;
    gLedger.playing.store(true);
    hardware_ = std::thread([this] { hardwareLoop(); });
    return S_OK;
  }
  HRESULT StopScheduledPlayback(BMDTimeValue, BMDTimeValue* actual, BMDTimeScale) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hardwareStop_ = true;
    }
    signal_.notify_all();
    if (hardware_.joinable()) hardware_.join();
    gLedger.playing.store(false);
    if (actual != nullptr) *actual = 0;
    return S_OK;
  }
  HRESULT IsScheduledPlaybackRunning(bool* active) override {
    *active = gLedger.playing.load();
    return S_OK;
  }
  HRESULT GetScheduledStreamTime(BMDTimeScale scale, BMDTimeValue* time, double* speed) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *time = playhead_ * mode_.duration * scale / mode_.scale;
    if (speed != nullptr) *speed = 1.0;
    return S_OK;
  }
  HRESULT GetReferenceStatus(BMDReferenceStatus* status) override {
    *status = 0;
    return S_OK;
  }
  HRESULT GetHardwareReferenceClock(BMDTimeScale, BMDTimeValue*, BMDTimeValue*,
                                    BMDTimeValue*) override {
    return E_NOTIMPL;
  }
  HRESULT GetFrameCompletionReferenceTimestamp(IDeckLinkVideoFrame*, BMDTimeScale,
                                               BMDTimeValue*) override {
    return E_NOTIMPL;
  }

  void resetSlots() {
    std::lock_guard<std::mutex> lock(mutex_);
    usedSlots_.clear();
  }

 private:
  struct Completion {
    FakeFrame* frame;
    BMDOutputFrameCompletionResult result;
  };

  /// One frame boundary per period: the least late due frame goes on air,
  /// anything it overtook is dropped, and what was on air before completes.
  void hardwareLoop() {
    const auto start = Clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000LL * mode_.duration / mode_.scale);
    for (int64_t k = 0;; ++k) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (signal_.wait_until(lock, start + period * k, [&] { return hardwareStop_; })) {
        return;
      }
      playhead_ = k;
      const BMDTimeValue now = k * mode_.duration;
      FakeFrame* show = nullptr;
      while (!queue_.empty() && queue_.begin()->first <= now) {
        if (show != nullptr) {
          pending_.push_back({show, bmdOutputFrameDropped});
        }
        show = queue_.begin()->second;
        queue_.erase(queue_.begin());
      }
      if (show != nullptr) {
        if (onAir_ != nullptr) {
          pending_.push_back({onAir_, onAirResult_});
        }
        onAir_ = show;
        onAirResult_ = show->displayTime == now ? bmdOutputFrameCompleted
                                                : bmdOutputFrameDisplayedLate;
      }
      signal_.notify_all();
    }
  }

  void completionLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      signal_.wait(lock, [&] { return quit_ || (!pending_.empty() && !completionsPaused_); });
      if (quit_) return;
      const Completion next = pending_.front();
      pending_.pop_front();
      IDeckLinkVideoOutputCallback* callback = callback_;
      if (callback != nullptr) callback->AddRef();
      delivering_ = true;
      lock.unlock();

      switch (next.result) {
        case bmdOutputFrameCompleted: ++gLedger.onTime; break;
        case bmdOutputFrameDisplayedLate: ++gLedger.late; break;
        case bmdOutputFrameDropped: ++gLedger.dropped; break;
        default: ++gLedger.flushed; break;
      }
      next.frame->scheduled.store(false);
      if (next.frame->external() != nullptr) {
        next.frame->external()->EndAccess(bmdBufferAccessRead);
      }
      if (callback != nullptr) {
        callback->ScheduledFrameCompleted(next.frame, next.result);
        callback->Release();
      }
      next.frame->Release();  // the card's reference: if it is the last, a frame dies here

      lock.lock();
      delivering_ = false;
      signal_.notify_all();
    }
  }

  std::atomic<ULONG> refs_{1};
  std::mutex mutex_;
  std::condition_variable signal_;
  std::thread completionThread_;
  std::thread hardware_;
  ModeInfo mode_ = kModes[1];
  bool videoEnabled_ = false;
  bool quit_ = false;
  bool hardwareStop_ = false;
  bool completionsPaused_ = false;
  bool delivering_ = false;
  int64_t playhead_ = 0;
  std::multimap<BMDTimeValue, FakeFrame*> queue_;
  std::deque<Completion> pending_;
  FakeFrame* onAir_ = nullptr;
  BMDOutputFrameCompletionResult onAirResult_ = bmdOutputFrameCompleted;
  IDeckLinkVideoOutputCallback* callback_ = nullptr;
  std::set<BMDTimeValue> usedSlots_;
};

FakeOutput* gOutput = nullptr;
FakeKeyer gKeyer;

class FakeDevice final : public IDeckLink {
 public:
  HRESULT QueryInterface(REFIID iid, LPVOID* out) override {
    if (sameIid(iid, IID_IDeckLinkOutput)) {
      gOutput->AddRef();
      *out = static_cast<IDeckLinkOutput*>(gOutput);
      return S_OK;
    }
    if (sameIid(iid, IID_IDeckLinkKeyer)) {
      gKeyer.AddRef();
      *out = static_cast<IDeckLinkKeyer*>(&gKeyer);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;  // no profile attributes: the keyer check is skipped
  }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT GetModelName(CFStringRef* name) override {
    *name = CFStringCreateWithCString(nullptr, "Fake Card", kCFStringEncodingUTF8);
    return S_OK;
  }
  HRESULT GetDisplayName(CFStringRef* name) override { return GetModelName(name); }

 private:
  ~FakeDevice() override = default;
  std::atomic<ULONG> refs_{1};
};

class FakeIterator final : public IDeckLinkIterator {
 public:
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs_; }
  ULONG Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT Next(IDeckLink** device) override {
    if (given_) {
      *device = nullptr;
      return S_FALSE;
    }
    given_ = true;
    *device = new FakeDevice();
    return S_OK;
  }

 private:
  ~FakeIterator() = default;  // IDeckLinkIterator declares no virtual destructor
  std::atomic<ULONG> refs_{1};
  bool given_ = false;
};

int gFailures = 0;
void expect(bool ok, const char* what) {
  std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++gFailures;
}

int64_t field(const weblinked::json::Value& status, const char* key) {
  return status[key].asInt64(-1);
}

/// Submits at the frame rate, as the engine's clock does, for `frames` ticks.
void run(weblinked::IOutput& output, const weblinked::VideoFrame& frame, int frames,
         std::chrono::nanoseconds period) {
  auto next = Clock::now();
  for (int i = 0; i < frames; ++i) {
    next += period;
    output.submit(frame, weblinked::AudioBlock{});
    std::this_thread::sleep_until(next);
  }
}

}  // namespace

extern "C" IDeckLinkIterator* CreateDeckLinkIteratorInstance(void) {
  return new FakeIterator();
}

int main() {
  using namespace weblinked;
  FakeOutput card;
  gOutput = &card;

  const auto format = *VideoFormat::parse("1080p50");
  const auto period = std::chrono::nanoseconds(20'000'000);

  for (const bool keyed : {false, true}) {
    std::printf("\n%s\n", keyed ? "keyed, premultiplied fill (BGRA)"
                                : "unkeyed (UYVY)");
    OutputSpec spec;
    spec.kind = "decklink";
    spec.name = "decklink0";
    spec.options = json::Value::object();
    if (keyed) {
      spec.options.set("keying", json::Value("external"));
      spec.options.set("premultiplied_fill", json::Value(true));
    }
    auto output = createDeckLinkOutput(spec);
    VideoFrame frame(format, output->pixelFormat());

    std::string error;
    const int64_t createdBefore = gLedger.created.load();
    // Counted per output: the last one's buffers are freed with it, and a new
    // allocation may land at an old address.
    const size_t buffersBefore = 0;
    {
      std::lock_guard<std::mutex> lock(gLedger.buffersMutex);
      gLedger.buffers.clear();
    }
    expect(output->start(format, error), "start");
    if (!error.empty()) std::printf("    %s\n", error.c_str());
    json::Value status = output->status();
    expect(field(status, "ring_size") == 10, "ring of 2 x preroll + 4 frames, made at start");
    expect(gLedger.created.load() - createdBefore == 10, "exactly the ring's frames created");
    expect(status["frame_memory"].asString() == "app", "frames wrap the output's own memory");

    // Steady playback.
    run(*output, frame, 150, period);
    status = output->status();
    std::printf("    3 s steady: frames=%lld completed=%lld late=%lld dropped=%lld "
                "buffered=%lld ring_free=%lld misses=%lld\n",
                (long long)field(status, "frames"), (long long)field(status, "frames_completed"),
                (long long)field(status, "frames_late"), (long long)field(status, "frames_dropped"),
                (long long)field(status, "buffered_frames"), (long long)field(status, "ring_free"),
                (long long)field(status, "ring_misses"));
    expect(field(status, "frames_late") == 0 && field(status, "frames_dropped") == 0,
           "steady: nothing late or dropped");
    expect(field(status, "ring_misses") == 0, "steady: the ring never ran dry");
    expect(field(status, "reanchors") == 0, "steady: no re-anchor");

    // A stall shorter than the queue: two frames.
    std::this_thread::sleep_for(period * 2);
    run(*output, frame, 50, period);
    status = output->status();
    expect(field(status, "reanchors") == 0, "two-frame stall: no re-anchor");

    // A stall longer than the queue, as SIGSTOP makes one: the completion
    // thread stops too, the hardware does not.
    const int64_t lateBefore = field(status, "frames_late");
    const int64_t droppedBefore = field(status, "frames_dropped");
    card.pauseCompletions(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // 75 frames
    card.pauseCompletions(false);
    run(*output, frame, 250, period);  // 5 s after
    status = output->status();
    std::printf("    after a 1.5 s stall and 5 s: reanchors=%lld reanchored_frames=%lld "
                "late=%lld dropped=%lld misses=%lld\n",
                (long long)field(status, "reanchors"),
                (long long)field(status, "reanchored_frames"),
                (long long)(field(status, "frames_late") - lateBefore),
                (long long)(field(status, "frames_dropped") - droppedBefore),
                (long long)field(status, "ring_misses"));
    expect(field(status, "reanchors") == 1, "long stall: re-anchored exactly once");
    expect(field(status, "frames_late") - lateBefore == 0 &&
               field(status, "frames_dropped") - droppedBefore == 0,
           "long stall: nothing late or dropped afterwards");

    output->stop();
    card.resetSlots();
    expect(gLedger.created.load() == gLedger.destroyed.load(),
           "stop: every frame released, once");

    // Disable/enable, as Daniel's supervisor and operators do it.
    for (int cycle = 0; cycle < 20; ++cycle) {
      if (!output->start(format, error)) {
        std::printf("    cycle %d: %s\n", cycle, error.c_str());
        ++gFailures;
        break;
      }
      run(*output, frame, 10, period);
      output->stop();
      card.resetSlots();
    }
    expect(gLedger.created.load() == gLedger.destroyed.load(),
           "20 stop/start cycles: nothing left behind");
    status = output->status();
    size_t buffersAfter = 0;
    {
      std::lock_guard<std::mutex> lock(gLedger.buffersMutex);
      buffersAfter = gLedger.buffers.size();
    }
    std::printf("    pixel buffers handed to the card over 21 starts: %zu; "
                "buffer_allocations=%lld buffer_overlaps=%lld\n",
                buffersAfter - buffersBefore,
                (long long)field(status, "buffer_allocations"),
                (long long)field(status, "buffer_overlaps"));
    expect(buffersAfter - buffersBefore == 10 && field(status, "buffer_allocations") == 1,
           "the same 10 buffers across every stop/start");
    expect(field(status, "buffer_overlaps") == 0,
           "no write while the card was reading a buffer");
  }

  std::printf("\nacross everything:\n");
  std::printf("    frames created %lld, destroyed %lld; on time %lld, late %lld, "
              "dropped %lld, flushed %lld\n",
              (long long)gLedger.created.load(), (long long)gLedger.destroyed.load(),
              (long long)gLedger.onTime.load(), (long long)gLedger.late.load(),
              (long long)gLedger.dropped.load(), (long long)gLedger.flushed.load());
  std::printf("    frames of SDK pool memory %lld, wrapping application memory %lld\n",
              (long long)gLedger.poolFrames.load(), (long long)gLedger.appFrames.load());
  expect(gLedger.poolFrames.load() == 0, "nothing taken from the SDK's allocator pool (#12)");
  expect(gLedger.createdWhilePlaying.load() == 0, "no pool frame created while playing");
  expect(gLedger.destroyedWhilePlaying.load() == 0, "no pool frame destroyed while playing");
  expect(gLedger.destroyedOnCompletionThread.load() == 0,
         "no pool frame destroyed on the completion thread (#12)");
  expect(gLedger.writesWhileScheduled.load() == 0, "no frame written while the card had it");
  expect(gLedger.slotsScheduledTwice.load() == 0, "no slot scheduled twice");

  card.shutdown();
  std::printf("\n%s\n", gFailures == 0 ? "PASS" : "FAIL");
  return gFailures == 0 ? 0 : 1;
}
