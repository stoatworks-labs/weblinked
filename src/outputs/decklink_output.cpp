// Blackmagic DeckLink output.
//
// The card is driven with scheduled playback rather than DisplayVideoFrameSync:
// scheduled playback is the only mode that produces genlocked, correctly timed
// SDI, and it is the mode that tolerates the engine's frame clock being a
// software clock. We pre-roll a few frames, start playback, and then top the
// queue up on every tick. The card's own buffer absorbs the difference between
// our clock and its clock; if that difference is one-directional the buffer
// eventually empties or overflows, which is why bufferedFrames appears in
// status() — it is the number to watch.
//
// Ownership follows COM rules: everything obtained from the API is Release()d,
// and the completion callback is reference counted independently of this class.
//
// Frames are created once, at start, into a ring, and recycled as the card hands
// them back — never created per tick and never destroyed by the completion
// thread. Creating one per tick let the SDK's buffer allocator be emptied on the
// completion thread while the clock thread was inside CreateVideoFrame, and on
// Desktop Video 16.4 that deadlocks inside the SDK (issue #12). On SDK 14.3 and
// later the pixel memory is WebLinked's own, wrapped with
// CreateVideoFrameWithBuffer, so the SDK's allocator pool is not used at all,
// and it is kept across a stop and start at the same raster. The display time
// of each frame comes from PlayoutSchedule, in the display mode's own exact time
// base, and is moved forward when the card's playhead has overtaken it (#13).
//
// Verified against a DeckLink Duo 2 since v0.6.1 (docs/04-verification.md,
// sections 19 and 19a). The ring and the re-anchoring are not yet: no card was
// attached when they were written, and they await a run on the rig that found
// both bugs. See section 40.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "DeckLinkAPI.h"
#include "DeckLinkAPIVersion.h"
#include "core/pixel_convert.h"
#include "core/playout_schedule.h"
#include "core/recycle_ring.h"
#include "diag/diag.h"
#include "outputs/output.h"

namespace weblinked {
namespace {

// The SDK spells the platform differences out in its own headers; these wrap
// the three that reach our code.
#if defined(_WIN32)
using DeckLinkBool = BOOL;
std::string deckLinkStringToStd(BSTR text) {
  if (text == nullptr) {
    return {};
  }
  const int length = ::SysStringLen(text);
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                          nullptr, nullptr);
  std::string out(static_cast<size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr,
                        nullptr);
  ::SysFreeString(text);
  return out;
}
#elif defined(__APPLE__)
using DeckLinkBool = bool;
std::string deckLinkStringToStd(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(text);
  const CFIndex maxBytes =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(maxBytes), '\0');
  std::string result;
  if (CFStringGetCString(text, out.data(), maxBytes, kCFStringEncodingUTF8)) {
    result = out.c_str();
  }
  CFRelease(text);
  return result;
}
#else
using DeckLinkBool = bool;
std::string deckLinkStringToStd(const char* text) {
  if (text == nullptr) {
    return {};
  }
  std::string out(text);
  ::free(const_cast<char*>(text));  // the Linux SDK returns malloc'd strings
  return out;
}
#endif

template <typename T>
void safeRelease(T*& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

/// Write access to a frame's pixels. SDK 14.3 moved the bytes off the frame onto
/// IDeckLinkVideoBuffer, which must be bracketed by StartAccess/EndAccess; older
/// SDKs hand them out directly. `data()` is null when the card refused.
class FrameBytes {
 public:
  explicit FrameBytes(IDeckLinkMutableVideoFrame* frame) {
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer,
                              reinterpret_cast<void**>(&buffer_)) != S_OK ||
        buffer_ == nullptr) {
      buffer_ = nullptr;
      return;
    }
    open();
#else
    if (frame->GetBytes(&bytes_) != S_OK) {
      bytes_ = nullptr;
    }
#endif
  }

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  /// A buffer WebLinked owns, written directly rather than through the frame
  /// that wraps it — but bracketed the same way, so the SDK and the overlap
  /// count in AppVideoBuffer see every write.
  explicit FrameBytes(IDeckLinkVideoBuffer* buffer) : buffer_(buffer) {
    buffer_->AddRef();
    open();
  }
#endif

  ~FrameBytes() {
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    if (buffer_ != nullptr) {
      if (accessing_) {
        buffer_->EndAccess(bmdBufferAccessWrite);
      }
      buffer_->Release();
    }
#endif
  }

  FrameBytes(const FrameBytes&) = delete;
  FrameBytes& operator=(const FrameBytes&) = delete;

  void* data() const { return bytes_; }

 private:
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  void open() {
    if (buffer_->StartAccess(bmdBufferAccessWrite) != S_OK) {
      buffer_->Release();
      buffer_ = nullptr;
      return;
    }
    accessing_ = true;
    if (buffer_->GetBytes(&bytes_) != S_OK) {
      bytes_ = nullptr;
    }
  }
#endif

  void* bytes_ = nullptr;
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  IDeckLinkVideoBuffer* buffer_ = nullptr;
  bool accessing_ = false;
#endif
};

/// Every DeckLink output in the process draws its frames from the SDK, and the
/// deadlock in #12 was the SDK's allocator pool being emptied on two threads at
/// once. Rings are only ever filled and emptied under this, so two outputs
/// starting or stopping together cannot reproduce it between them. Timed: a
/// driver wedged inside one card's CreateVideoFrame must not stop every other
/// DeckLink, in every source, from starting or stopping too. Namespace scope
/// rather than a function-local static: the engine is built with
/// -fno-threadsafe-statics.
std::timed_mutex gFrameAllocationMutex;
constexpr auto kAllocationWait = std::chrono::seconds(5);

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
/// Pixel memory WebLinked owns, given to the SDK with CreateVideoFrameWithBuffer.
///
/// The point is what it keeps out: a frame made this way has no buffer from the
/// SDK's VideoBufferAllocatorPool, so nothing WebLinked does can empty that
/// pool — not a frame released on the completion thread, not a ring released
/// at a stop. That pool emptying under a concurrent allocation is where #12
/// deadlocked. The contract is the SDK manual's (2.5.53) and its KeyerOutput
/// sample's: at least RowBytesForPixelFormat x height bytes, GetBytes giving
/// the CPU address, StartAccess/EndAccess counted per kind of access.
///
/// It also keeps count of one thing worth knowing on a real card: a write
/// starting while a read is open, or the reverse. The ring never writes a
/// frame the card has, so if the SDK brackets its own reads, this stays 0.
class AppVideoBuffer final : public IDeckLinkVideoBuffer {
 public:
  static AppVideoBuffer* create(size_t size) {
    // Page-aligned, and rounded up to a page: memory a card transfers from.
    constexpr size_t kAlignment = 4096;
    const size_t rounded = (size + kAlignment - 1) / kAlignment * kAlignment;
    void* bytes = ::operator new(rounded, std::align_val_t(kAlignment), std::nothrow);
    if (bytes == nullptr) {
      return nullptr;
    }
    AppVideoBuffer* buffer = new (std::nothrow) AppVideoBuffer(bytes, size);
    if (buffer == nullptr) {
      ::operator delete(bytes, std::align_val_t(kAlignment));
    }
    return buffer;
  }

  HRESULT QueryInterface(REFIID iid, LPVOID* out) override {
    // IUnknown's IID, 00000000-0000-0000-C000-000000000046, as bytes: the same
    // sixteen on every platform, where the SDK spells the constant three ways.
    static const uint8_t kUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                         0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    if (out == nullptr) {
      return E_INVALIDARG;
    }
    if (std::memcmp(&iid, &IID_IDeckLinkVideoBuffer, sizeof(kUnknown)) == 0 ||
        std::memcmp(&iid, kUnknown, sizeof(kUnknown)) == 0) {
      *out = static_cast<IDeckLinkVideoBuffer*>(this);
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
      delete this;
    }
    return left;
  }

  HRESULT GetBytes(void** buffer) override {
    if (buffer == nullptr) {
      return E_INVALIDARG;
    }
    *buffer = bytes_;
    return S_OK;
  }
  HRESULT GetSize(uint64_t* size) override {
    if (size == nullptr) {
      return E_INVALIDARG;
    }
    *size = size_;
    return S_OK;
  }
  HRESULT StartAccess(BMDBufferAccessFlags flags) override {
    const bool reading = (flags & bmdBufferAccessRead) != 0;
    const bool writing = (flags & bmdBufferAccessWrite) != 0;
    if ((writing && readers_.load() > 0) || (reading && writers_.load() > 0)) {
      ++overlaps_;
    }
    if (reading) ++readers_;
    if (writing) ++writers_;
    return S_OK;
  }
  HRESULT EndAccess(BMDBufferAccessFlags flags) override {
    const bool reading = (flags & bmdBufferAccessRead) != 0;
    const bool writing = (flags & bmdBufferAccessWrite) != 0;
    if ((reading && readers_.load() <= 0) || (writing && writers_.load() <= 0)) {
      return E_INVALIDARG;  // the manual's answer to an unmatched EndAccess
    }
    if (reading) --readers_;
    if (writing) --writers_;
    return S_OK;
  }

  int64_t overlaps() const { return overlaps_.load(); }

 private:
  AppVideoBuffer(void* bytes, size_t size) : bytes_(bytes), size_(size) {}
  // Private, and virtual through the interface: only Release() destroys one.
  ~AppVideoBuffer() override {
    ::operator delete(bytes_, std::align_val_t(4096));
  }

  void* bytes_;
  size_t size_;
  std::atomic<ULONG> refs_{1};
  std::atomic<int> readers_{0};
  std::atomic<int> writers_{0};
  std::atomic<int64_t> overlaps_{0};
};
#endif

using FrameRing = RecycleRing<IDeckLinkMutableVideoFrame>;

/// What the completion thread reports, shared with the output. Cumulative
/// across restarts, as the counters always were.
struct PlayoutCounters {
  std::atomic<int64_t> completed{0};
  std::atomic<int64_t> late{0};
  std::atomic<int64_t> dropped{0};
  std::atomic<int64_t> flushed{0};
  /// Completions for a frame the ring did not recognise, or already had back.
  std::atomic<int64_t> strays{0};
  /// Set by a late or dropped completion, cleared by the clock thread when it
  /// next asks where the card's playhead is.
  std::atomic<bool> lateSinceCheck{false};
};

/// Notifies us when the card has finished with a frame, which is where frames
/// come back to the ring — the SDK's own documented place to reuse them — and
/// the only place the card says it is running late or has run dry.
///
/// Holds the counters and the ring by shared_ptr: the SDK owns a reference to
/// this object, and a completion that arrived after the output had gone must
/// find memory that still exists.
class CompletionCallback final : public IDeckLinkVideoOutputCallback {
 public:
  CompletionCallback(std::shared_ptr<PlayoutCounters> counters,
                     std::shared_ptr<FrameRing> ring)
      : counters_(std::move(counters)), ring_(std::move(ring)) {}

  HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame,
                                  BMDOutputFrameCompletionResult result) override {
    ++counters_->completed;
    switch (result) {
      case bmdOutputFrameDisplayedLate:
        ++counters_->late;
        counters_->lateSinceCheck.store(true);
        break;
      case bmdOutputFrameDropped:
        ++counters_->dropped;
        counters_->lateSinceCheck.store(true);
        break;
      case bmdOutputFrameFlushed:
        ++counters_->flushed;
        break;
      default:
        break;
    }
    // Back to the ring, and not Release()d: the ring holds our reference for
    // as long as the output runs, so the SDK's own release after this call
    // returns can never be the last one. That is the whole fix for #12 — no
    // frame is destroyed on this thread, so this thread never reaches the
    // allocator.
    if (completedFrame != nullptr && !ring_->release(completedFrame)) {
      ++counters_->strays;
    }
    return S_OK;
  }

  HRESULT ScheduledPlaybackHasStopped() override { return S_OK; }

  // IUnknown. Hand-rolled because the SDK's objects are COM on every platform.
  HRESULT QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refCount_; }
  ULONG Release() override {
    const ULONG count = --refCount_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

 private:
  ~CompletionCallback() override = default;

  std::shared_ptr<PlayoutCounters> counters_;
  std::shared_ptr<FrameRing> ring_;
  std::atomic<ULONG> refCount_{1};
};

/// What the card should do with the page's alpha channel.
enum class Keying {
  /// Ignore alpha; composite over black and send 4:2:2 YUV.
  kOff,
  /// Fill and key on two SDI connectors, for a downstream keyer or vision
  /// mixer. This is what "key + fill" means on a broadcast card.
  kExternal,
  /// The card composites our frame over its own SDI *input* and sends the
  /// result. No downstream keyer needed, but it costs an input.
  kInternal,
};

Keying parseKeying(const std::string& text) {
  if (text == "external" || text == "keyfill" || text == "key+fill") {
    return Keying::kExternal;
  }
  if (text == "internal") {
    return Keying::kInternal;
  }
  return Keying::kOff;
}

const char* keyingToString(Keying keying) {
  switch (keying) {
    case Keying::kExternal: return "external";
    case Keying::kInternal: return "internal";
    case Keying::kOff:      return "off";
  }
  return "off";
}

class DeckLinkOutput final : public IOutput {
 public:
  explicit DeckLinkOutput(const OutputSpec& spec)
      : IOutput(spec.name.empty() ? "decklink" : spec.name),
        deviceIndex_(spec.deviceIndex),
        prerollFrames_(std::max(2, spec.optionInt("preroll", 3))),
        // The ring has to cover everything the card holds at once, which is
        // more than the preroll: a Duo 2 at preroll 3 sat at 6 buffered frames
        // for minutes on end (sections 19 and 19a). Twice the preroll plus four
        // leaves room over that; the option is there for a card that needs more.
        ringFrames_(std::clamp(spec.optionInt("ring", 2 * prerollFrames_ + 4),
                               prerollFrames_ + 2, std::max(64, prerollFrames_ + 2))),
        audioChannels_(spec.optionInt("audio_channels", 2)),
        keying_(parseKeying(spec.optionString("keying", "off"))),
        keyLevel_(static_cast<uint8_t>(
            std::clamp(spec.optionInt("key_level", 255), 0, 255))),
        premultipliedFill_(spec.optionBool("premultiplied_fill", false)) {}

  ~DeckLinkOutput() override {
    stop();
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    releaseBuffers();
#endif
  }

  std::string kind() const override { return "decklink"; }

  /// 8-bit YUV (UYVY on the wire) is the card's native SDI format; handing it
  /// BGRA would make the driver convert on the CPU behind our back.
  ///
  /// Keying is the exception, and it is not optional: 4:2:2 YUV has nowhere to
  /// put an alpha channel, so a keyed output has to be fed 8-bit BGRA and let
  /// the card derive fill and key from it.
  PixelFormat pixelFormat() const override {
    return keying_ == Keying::kOff ? PixelFormat::kUYVY : PixelFormat::kBGRA;
  }

  /// Chromium paints premultiplied. A keyer set to straight alpha needs that
  /// undone; one set to premultiplied key (an ATEM DSK with Pre Multiplied
  /// Key on) takes Chromium's buffer as it is.
  bool wantsStraightAlpha() const override {
    return keying_ != Keying::kOff && !premultipliedFill_;
  }

  BMDPixelFormat cardPixelFormat() const {
    return keying_ == Keying::kOff ? bmdFormat8BitYUV : bmdFormat8BitBGRA;
  }

  bool start(const VideoFormat& format, std::string& error) override {
    stop();
    format_ = format;

    if (!openDevice(error)) {
      stop();
      return false;
    }
    if (!findDisplayMode(format, error)) {
      stop();
      return false;
    }

    if (!configureKeyer(error)) {
      stop();
      return false;
    }

    if (output_->EnableVideoOutput(displayMode_, bmdVideoOutputFlagDefault) != S_OK) {
      error = "EnableVideoOutput failed for " + format.toString() +
              " on '" + deviceName_ + "' (is another application using it?)";
      stop();
      return false;
    }
    videoEnabled_ = true;

    if (!buildRing(error)) {
      stop();
      return false;
    }
    schedule_ = PlayoutSchedule(modeFrameDuration_, modeTimeScale_, prerollFrames_);

    callback_ = new CompletionCallback(counters_, ring_);
    if (output_->SetScheduledFrameCompletionCallback(callback_) != S_OK) {
      error = "SetScheduledFrameCompletionCallback failed";
      stop();
      return false;
    }

    if (audioChannels_ > 0) {
      // 32-bit integer PCM: our source is float, so the wider target avoids
      // throwing away resolution we already have.
      if (output_->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                     bmdAudioSampleType32bitInteger,
                                     static_cast<uint32_t>(audioChannels_),
                                     bmdAudioOutputStreamContinuous) != S_OK) {
        // Not fatal: a video-only feed is better than no feed.
        audioEnabled_ = false;
      } else {
        audioEnabled_ = true;
      }
    }

    // Pre-roll black so playback starts from a defined state rather than from
    // whatever the previous application left in the card's memory.
    for (int i = 0; i < prerollFrames_; ++i) {
      if (!scheduleBlackFrame(error)) {
        stop();
        return false;
      }
    }

    if (output_->StartScheduledPlayback(0, schedule_.timeScale(), 1.0) != S_OK) {
      error = "StartScheduledPlayback failed on '" + deviceName_ + "'";
      stop();
      return false;
    }
    playing_ = true;
    running_ = true;
    return true;
  }

  /// In the order the SDK's own samples use. DisableVideoOutput blocks until
  /// every scheduled frame has completed or been flushed, and it runs with the
  /// callback still installed, so each frame comes back to the ring first. Only
  /// then is the callback removed and the ring emptied: by that point the card
  /// holds no frame, the completion thread has nothing left to do, and the last
  /// release of every frame happens here, on one thread.
  void stop() override {
    if (output_ != nullptr) {
      if (playing_) {
        BMDTimeValue actualStop = 0;
        output_->StopScheduledPlayback(0, &actualStop, schedule_.timeScale());
        playing_ = false;
      }
      if (videoEnabled_) {
        output_->DisableVideoOutput();
        videoEnabled_ = false;
      }
      if (audioEnabled_) {
        output_->DisableAudioOutput();
        audioEnabled_ = false;
      }
      output_->SetScheduledFrameCompletionCallback(nullptr);
    }
    if (keyer_ != nullptr) {
      keyer_->Disable();
    }
    releaseRing();
    safeRelease(keyer_);
    safeRelease(callback_);
    safeRelease(output_);
    safeRelease(device_);
    running_ = false;
    schedule_.reset();
    framesScheduled_ = 0;
    scheduledAudioFrames_ = 0;
  }

  void submit(const VideoFrame& video, const AudioBlock& audio) override {
    if (!running_ || output_ == nullptr || ring_ == nullptr) {
      return;
    }
    // Rule 5: never copy a frame into a buffer sized for another raster. The
    // engine already refuses to send one; this is the half that does not
    // depend on the engine being right.
    if (video.format().width != format_.width ||
        video.format().height != format_.height ||
        video.pixelFormat() != pixelFormat()) {
      ++refusedFrames_;
      return;
    }

    // A free frame, or none: never wait for the card to hand one back. Every
    // other output on this clock would wait too. The ring counts the miss.
    IDeckLinkMutableVideoFrame* frame = ring_->acquire();
    if (frame == nullptr) {
      return;
    }

    // Written only between the card handing the frame back and it being
    // scheduled again, and bracketed by FrameBytes, which on SDK 14.3 and later
    // is StartAccess/EndAccess on the frame's IDeckLinkVideoBuffer. The access
    // ends with this block, before the frame is scheduled.
    bool written = false;
    if (FrameBytes access = bytesOf(frame); void* bytes = access.data()) {
      const int cardStride = frame->GetRowBytes();
      const int ourStride = video.rowBytes();
      if (cardStride == ourStride) {
        std::memcpy(bytes, video.data(),
                    static_cast<size_t>(ourStride) * format_.height);
      } else {
        // The card is entitled to a wider stride than the picture.
        for (int y = 0; y < format_.height; ++y) {
          std::memcpy(static_cast<uint8_t*>(bytes) + static_cast<size_t>(y) * cardStride,
                      video.data() + static_cast<size_t>(y) * ourStride,
                      static_cast<size_t>(ourStride));
        }
      }
      written = true;
    }
    if (!written) {
      // A recycled frame still holds a picture from several frames ago, so
      // one the card would not let us write is not sent at all.
      ++accessFailures_;
      ring_->release(frame);
      return;
    }

    if (!scheduleFrame(frame)) {
      return;
    }

    if (audioEnabled_ && audio.valid() && audio.interleaved != nullptr) {
      scheduleAudio(audio);
    }
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("decklink"));
    value.set("name", json::Value(name_));
    value.set("running", json::Value(running_));
    value.set("device", json::Value(deviceName_));
    value.set("device_index", json::Value(deviceIndex_));
    value.set("frames", json::Value(framesScheduled_));
    value.set("audio_frames", json::Value(scheduledAudioFrames_));
    value.set("frames_completed", json::Value(counters_->completed.load()));
    // As the card reports them, in completions: a frame shown after its time,
    // and one never shown because a less late one was available.
    value.set("frames_late", json::Value(counters_->late.load()));
    value.set("frames_dropped", json::Value(counters_->dropped.load()));
    value.set("frames_flushed", json::Value(counters_->flushed.load()));
    value.set("create_failures", json::Value(createFailures_));
    value.set("schedule_failures", json::Value(scheduleFailures_));
    // The frame ring (#12). ring_misses counts ticks dropped here because the
    // card still had every frame — never waited for, because waiting would
    // stall every other output on this clock.
    value.set("ring_size", json::Value(ring_ != nullptr
                                           ? static_cast<int>(ring_->size())
                                           : 0));
    value.set("ring_free", json::Value(ring_ != nullptr
                                           ? static_cast<int>(ring_->freeCount())
                                           : 0));
    value.set("ring_misses",
              json::Value(ringMisses_ + (ring_ != nullptr ? ring_->misses() : 0)));
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    // Whose memory the frames are in: ours, so the SDK's allocator pool is out
    // of it. buffer_allocations counts how often the ring's memory was made —
    // once per raster, not once per start. buffer_overlaps counts a write and a
    // read of one buffer open at once; the ring never writes a frame the card
    // has, so a non-zero value means the SDK reads at a time this did not
    // expect, and is worth reporting.
    value.set("frame_memory", json::Value("app"));
    value.set("buffer_allocations", json::Value(bufferAllocations_));
    value.set("buffer_overlaps", json::Value(liveOverlaps()));
#else
    value.set("frame_memory", json::Value("sdk"));
#endif
    // Re-anchoring (#13): how often the schedule had fallen behind the card's
    // playhead and was moved forward, and how many frame slots that skipped.
    value.set("reanchors", json::Value(reanchors_));
    value.set("reanchored_frames", json::Value(reanchoredFrames_));
    if (refusedFrames_ != 0 || accessFailures_ != 0 || counters_->strays.load() != 0) {
      value.set("frames_refused", json::Value(refusedFrames_));
      value.set("access_failures", json::Value(accessFailures_));
      value.set("stray_completions", json::Value(counters_->strays.load()));
    }
    value.set("keying", json::Value(keyingToString(keying_)));
    if (keying_ != Keying::kOff) {
      value.set("key_level", json::Value(static_cast<int>(keyLevel_)));
      value.set("premultiplied_fill", json::Value(premultipliedFill_));
      value.set("keyer_active", json::Value(keyer_ != nullptr));
    }

    if (output_ != nullptr) {
      uint32_t buffered = 0;
      if (output_->GetBufferedVideoFrameCount(&buffered) == S_OK) {
        // The number to watch: a steady value means our clock and the card's
        // clock agree. A drift in either direction ends in a glitch.
        value.set("buffered_frames", json::Value(static_cast<int>(buffered)));
      }
      uint32_t bufferedAudio = 0;
      if (audioEnabled_ &&
          output_->GetBufferedAudioSampleFrameCount(&bufferedAudio) == S_OK) {
        value.set("buffered_audio_samples",
                  json::Value(static_cast<int>(bufferedAudio)));
      }
      BMDReferenceStatus reference = static_cast<BMDReferenceStatus>(0);
      if (output_->GetReferenceStatus(&reference) == S_OK) {
        value.set("genlocked",
                  json::Value((reference & bmdReferenceLocked) != 0));
      }
    }
    return value;
  }

 private:
  bool openDevice(std::string& error) {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (iterator == nullptr) {
      error = "DeckLink drivers not present (Desktop Video is not installed)";
      return false;
    }

    int index = 0;
    IDeckLink* candidate = nullptr;
    while (iterator->Next(&candidate) == S_OK) {
      if (index == deviceIndex_) {
        device_ = candidate;
        break;
      }
      candidate->Release();
      ++index;
    }
    iterator->Release();

    if (device_ == nullptr) {
      error = "no DeckLink device at index " + std::to_string(deviceIndex_) +
              " (found " + std::to_string(index) + ")";
      return false;
    }

#if defined(_WIN32)
    BSTR nameText = nullptr;
#elif defined(__APPLE__)
    CFStringRef nameText = nullptr;
#else
    const char* nameText = nullptr;
#endif
    if (device_->GetDisplayName(&nameText) == S_OK) {
      deviceName_ = deckLinkStringToStd(nameText);
    }

    if (device_->QueryInterface(IID_IDeckLinkOutput,
                                reinterpret_cast<void**>(&output_)) != S_OK) {
      error = "'" + deviceName_ + "' has no output interface (capture-only card?)";
      return false;
    }
    return true;
  }

  /// Matches our raster and rate against what the card offers.
  ///
  /// Iterating the modes rather than mapping to a bmdMode* constant is
  /// deliberate: the constant list grows with every SDK release, and a card
  /// that supports a mode this build has never heard of should still work.
  bool findDisplayMode(const VideoFormat& format, std::string& error) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
      error = "GetDisplayModeIterator failed";
      return false;
    }

    std::string offered;
    IDeckLinkDisplayMode* mode = nullptr;
    bool found = false;
    while (iterator->Next(&mode) == S_OK) {
      BMDTimeValue duration = 0;
      BMDTimeScale scale = 0;
      mode->GetFrameRate(&duration, &scale);

      const bool sizeMatches = mode->GetWidth() == format.width &&
                               mode->GetHeight() == format.height;
      // Compare as a cross-multiplied rational: duration/scale is the frame
      // period, so scale/duration is the rate.
      const bool rateMatches =
          duration > 0 &&
          static_cast<int64_t>(scale) * format.rate.denominator ==
              static_cast<int64_t>(duration) * format.rate.numerator;

      const BMDFieldDominance dominance = mode->GetFieldDominance();
      const bool modeInterlaced = dominance == bmdUpperFieldFirst ||
                                  dominance == bmdLowerFieldFirst;
      const bool scanMatches = modeInterlaced == format.interlaced;

      if (sizeMatches && rateMatches && scanMatches) {
        displayMode_ = mode->GetDisplayMode();
        // The card's own time base for this mode — 1000/25000 for 1080p25,
        // 1001/60000 for 59.94 — so that every display time is an exact
        // multiple of it (rule 2).
        modeFrameDuration_ = duration;
        modeTimeScale_ = scale;
        found = true;
        mode->Release();
        break;
      }

      if (sizeMatches && !offered.empty()) {
        offered += ", ";
      }
      if (sizeMatches) {
        offered += std::to_string(scale) + "/" + std::to_string(duration);
      }
      mode->Release();
    }
    iterator->Release();

    if (!found) {
      error = "'" + deviceName_ + "' does not support " + format.toString();
      if (!offered.empty()) {
        error += " (it offers these rates at that raster: " + offered + ")";
      }
      return false;
    }

    // Ask the card directly as well: mode enumeration lists what the hardware
    // knows, not what this connection and pixel format can actually carry.
    DeckLinkBool supported = false;
    BMDDisplayMode actualMode = displayMode_;
    if (output_->DoesSupportVideoMode(bmdVideoConnectionUnspecified, displayMode_,
                                      cardPixelFormat(),
                                      bmdNoVideoOutputConversion,
                                      bmdSupportedVideoModeDefault, &actualMode,
                                      &supported) == S_OK &&
        !supported) {
      error = "'" + deviceName_ + "' reports " + format.toString() +
              " as unsupported for " +
              (keying_ == Keying::kOff ? "8-bit YUV" : "8-bit BGRA") + " output";
      if (keying_ != Keying::kOff) {
        // Keying needs an alpha-carrying format, and RGBA costs twice the link
        // bandwidth of the 4:2:2 the same raster fits in. On a DeckLink Duo 2
        // that is the whole difference between 1080p50 and 1080p25: the card
        // reports keying support and every rate as available, then refuses BGRA
        // at the top one. Naming what *would* work turns "unsupported" into a
        // setting to change.
        error += ". Keying carries alpha, which needs about twice the link rate "
                 "of the same raster without it";
        const std::string usable = ratesSupportingAlpha(format);
        if (!usable.empty()) {
          error += "; this card accepts a keyed " +
                   std::to_string(format.width) + "x" +
                   std::to_string(format.height) + " at " + usable;
        }
      }
      return false;
    }
    return true;
  }

  /// Which rates at the *current* raster this card will accept with alpha.
  ///
  /// Asked of the hardware rather than assumed: DoesSupportVideoMode is the only
  /// thing that knows, and the answer differs by model, by profile and by
  /// raster.
  std::string ratesSupportingAlpha(const VideoFormat& format) {
    IDeckLinkDisplayModeIterator* iterator = nullptr;
    if (output_ == nullptr ||
        output_->GetDisplayModeIterator(&iterator) != S_OK || iterator == nullptr) {
      return {};
    }
    std::string usable;
    IDeckLinkDisplayMode* mode = nullptr;
    while (iterator->Next(&mode) == S_OK) {
      if (mode->GetWidth() == format.width &&
          mode->GetHeight() == format.height) {
        DeckLinkBool ok = false;
        if (output_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                          mode->GetDisplayMode(), bmdFormat8BitBGRA,
                                          bmdNoVideoOutputConversion,
                                          bmdSupportedVideoModeDefault, nullptr,
                                          &ok) == S_OK && ok) {
          BMDTimeValue duration = 0;
          BMDTimeScale scale = 0;
          mode->GetFrameRate(&duration, &scale);
          if (duration > 0) {
            if (!usable.empty()) usable += ", ";
            usable += std::to_string(scale) + "/" + std::to_string(duration);
            if (mode->GetFieldDominance() == bmdUpperFieldFirst ||
                mode->GetFieldDominance() == bmdLowerFieldFirst) {
              usable += " (interlaced)";
            }
          }
        }
      }
      mode->Release();
    }
    iterator->Release();
    return usable;
  }

  /// Turns the card's keyer on, having first checked it has one.
  ///
  /// Worth checking rather than just trying: keying is a hardware capability
  /// that varies by model and, on some cards, by the active profile. Enable()
  /// failing gives no reason, whereas the attribute flag lets us say which of
  /// the two kinds this card lacks — the difference between "buy a different
  /// card" and "switch to internal keying".
  bool configureKeyer(std::string& error) {
    if (keying_ == Keying::kOff) {
      return true;
    }

    IDeckLinkProfileAttributes* attributes = nullptr;
    if (device_->QueryInterface(IID_IDeckLinkProfileAttributes,
                                reinterpret_cast<void**>(&attributes)) == S_OK &&
        attributes != nullptr) {
      const BMDDeckLinkAttributeID attribute =
          keying_ == Keying::kExternal ? BMDDeckLinkSupportsExternalKeying
                                       : BMDDeckLinkSupportsInternalKeying;
      DeckLinkBool supported = false;
      const bool queried = attributes->GetFlag(attribute, &supported) == S_OK;
      attributes->Release();
      if (queried && !supported) {
        error = "'" + deviceName_ + "' does not support " +
                std::string(keyingToString(keying_)) + " keying";
        return false;
      }
    }

    if (device_->QueryInterface(IID_IDeckLinkKeyer,
                                reinterpret_cast<void**>(&keyer_)) != S_OK ||
        keyer_ == nullptr) {
      error = "'" + deviceName_ + "' has no keyer interface";
      return false;
    }

    // Enable(true) is external — fill and key on separate connectors.
    // Enable(false) is internal — composited over the card's own SDI input.
    if (keyer_->Enable(keying_ == Keying::kExternal) != S_OK) {
      error = "could not enable " + std::string(keyingToString(keying_)) +
              " keying on '" + deviceName_ + "'";
      safeRelease(keyer_);
      return false;
    }

    // The keyer's overall opacity, independent of per-pixel alpha. 255 means
    // the page's own alpha decides everything, which is what you want unless
    // you are deliberately fading the whole graphic.
    keyer_->SetLevel(keyLevel_);
    return true;
  }

  /// Creates every frame this run will use, while nothing else is running on
  /// this output. Our reference to each is held by the ring until
  /// releaseRing().
  /// Creates every frame this run will use, while nothing else is running on
  /// this output. Our reference to each is held by the ring until
  /// releaseRing().
  bool buildRing(std::string& error) {
    std::unique_lock<std::timed_mutex> allocation(gFrameAllocationMutex,
                                                  std::defer_lock);
    if (!allocation.try_lock_for(kAllocationWait)) {
      error = "another DeckLink output has been creating or releasing frames for "
              "over 5 s, so its driver may not be answering; '" + deviceName_ +
              "' was not started";
      return false;
    }
    auto ring = std::make_shared<FrameRing>();
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    if (!prepareBuffers(error)) {
      return false;
    }
#endif
    for (int i = 0; i < ringFrames_; ++i) {
      IDeckLinkMutableVideoFrame* frame = nullptr;
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
      AppVideoBuffer* buffer = buffers_[static_cast<size_t>(i)];
      const HRESULT made = output_->CreateVideoFrameWithBuffer(
          format_.width, format_.height, bufferRowBytes_, cardPixelFormat(),
          bmdFrameFlagDefault, buffer, &frame);
      const char* call = "CreateVideoFrameWithBuffer";
#else
      const HRESULT made = output_->CreateVideoFrame(
          format_.width, format_.height, format_.rowBytes(pixelFormat()),
          cardPixelFormat(), bmdFrameFlagDefault, &frame);
      const char* call = "CreateVideoFrame";
#endif
      if (made != S_OK || frame == nullptr) {
        ++createFailures_;
        error = std::string(call) + " failed for frame " + std::to_string(i + 1) +
                " of " + std::to_string(ringFrames_) + " on '" + deviceName_ + "'";
        for (IDeckLinkMutableVideoFrame* done : ring->close()) {
          done->Release();
        }
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
        frameBuffers_.clear();
#endif
        return false;
      }
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
      frameBuffers_.emplace_back(frame, buffer);
#endif
      ring->add(frame);
    }
    ring_ = std::move(ring);
    return true;
  }

#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  /// The ring's pixel memory: ours, one buffer per frame, kept from one start
  /// to the next while the raster and pixel format are the same — so an
  /// operator disabling and enabling an output, as #12's rig did, allocates and
  /// frees nothing at all. Only the frames wrapping them are made per start, and
  /// those take nothing from the SDK's pool.
  bool prepareBuffers(std::string& error) {
    int32_t rowBytes = 0;
    if (output_->RowBytesForPixelFormat(cardPixelFormat(), format_.width,
                                        &rowBytes) != S_OK ||
        rowBytes <= 0) {
      rowBytes = format_.rowBytes(pixelFormat());
    }
    const size_t bytes = static_cast<size_t>(rowBytes) *
                         static_cast<size_t>(format_.height);
    if (buffers_.size() == static_cast<size_t>(ringFrames_) &&
        bufferRowBytes_ == rowBytes && bufferBytes_ == bytes) {
      return true;
    }
    releaseBuffers();
    for (int i = 0; i < ringFrames_; ++i) {
      AppVideoBuffer* buffer = AppVideoBuffer::create(bytes);
      if (buffer == nullptr) {
        error = "could not allocate " + std::to_string(bytes) +
                " bytes for frame " + std::to_string(i + 1) + " of " +
                std::to_string(ringFrames_);
        releaseBuffers();
        return false;
      }
      buffers_.push_back(buffer);
    }
    bufferRowBytes_ = rowBytes;
    bufferBytes_ = bytes;
    ++bufferAllocations_;
    return true;
  }

  void releaseBuffers() {
    for (AppVideoBuffer* buffer : buffers_) {
      bufferOverlaps_ += buffer->overlaps();
      buffer->Release();  // ours; a frame still holding one keeps it alive
    }
    buffers_.clear();
    bufferRowBytes_ = 0;
    bufferBytes_ = 0;
  }

  /// The buffer a ring frame wraps. frameBuffers_ is fixed for the whole run,
  /// so this reads it without a lock.
  AppVideoBuffer* bufferFor(IDeckLinkMutableVideoFrame* frame) const {
    for (const auto& [wrapped, buffer] : frameBuffers_) {
      if (wrapped == frame) {
        return buffer;
      }
    }
    return nullptr;
  }

  int64_t liveOverlaps() const {
    int64_t total = bufferOverlaps_;
    for (const AppVideoBuffer* buffer : buffers_) {
      total += buffer->overlaps();
    }
    return total;
  }
#endif

  /// Our references to the frames go last, after DisableVideoOutput has had
  /// every frame back from the card — see stop(). On SDK 14.3 and later that
  /// releases only the SDK's frame objects, and our buffers stay for the next
  /// start; before 14.3 it is the last release of SDK-allocated memory, which
  /// is why it is serialised across outputs.
  void releaseRing() {
    if (ring_ == nullptr) {
      return;
    }
    ringMisses_ += ring_->misses();
    size_t outstanding = 0;
    const auto frames = ring_->close(&outstanding);
    if (outstanding != 0) {
      // The card did not hand everything back. Releasing ours is still right —
      // COM keeps a frame alive while the SDK holds it — but it means the
      // driver did not finish with them when it said it had.
      diag::warn("decklink '%s': %zu of %zu frames were still with the card at "
                 "stop", name_.c_str(), outstanding, frames.size());
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
      // And their memory must not be reused: the next start would wrap new
      // frames round buffers those old frames still wrap. Let go of ours; the
      // frames the SDK still holds keep their buffers alive until it lets go,
      // and the next start allocates afresh.
      releaseBuffers();
#endif
    }
    std::unique_lock<std::timed_mutex> allocation(gFrameAllocationMutex,
                                                  std::defer_lock);
    const bool serialised = allocation.try_lock_for(kAllocationWait);
#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0e030000
    if (!serialised) {
      // Before 14.3 these are the SDK's own buffers, and another output's
      // driver is wedged inside the allocator they go back to. Releasing them
      // now would walk into the same lock. Leaking them is the lesser harm.
      diag::error("decklink '%s': %zu frames not released, because another "
                  "DeckLink output has held the frame allocator for over 5 s",
                  name_.c_str(), frames.size());
      ring_.reset();
      return;
    }
#else
    (void)serialised;  // our own memory: no SDK allocator to wait for
#endif
    for (IDeckLinkMutableVideoFrame* frame : frames) {
      frame->Release();
    }
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    frameBuffers_.clear();
#endif
    ring_.reset();
  }

  /// Write access to a ring frame's pixels: our own buffer directly on SDK 14.3
  /// and later, the frame's otherwise.
  FrameBytes bytesOf(IDeckLinkMutableVideoFrame* frame) const {
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
    if (AppVideoBuffer* buffer = bufferFor(frame)) {
      return FrameBytes(static_cast<IDeckLinkVideoBuffer*>(buffer));
    }
#endif
    return FrameBytes(frame);
  }

  bool scheduleBlackFrame(std::string& error) {
    IDeckLinkMutableVideoFrame* frame = ring_->acquire();
    if (frame == nullptr) {
      error = "the frame ring is smaller than the preroll";
      return false;
    }
    if (FrameBytes access = bytesOf(frame); void* bytes = access.data()) {
      if (keying_ == Keying::kOff) {
        fillBlackUyvy(static_cast<uint8_t*>(bytes), frame->GetRowBytes(),
                      format_.width, format_.height);
      } else {
        // Pre-roll a *transparent* frame when keying: opaque black would punch
        // a black hole in whatever is behind the key for the first few frames.
        std::memset(bytes, 0,
                    static_cast<size_t>(frame->GetRowBytes()) * format_.height);
      }
    }
    if (!scheduleFrame(frame)) {
      error = "ScheduleVideoFrame failed while pre-rolling";
      return false;
    }
    return true;
  }

  /// Schedules at the next slot on the card's timeline.
  ///
  /// Display times come from a monotonically increasing frame index rather than
  /// from wall-clock arithmetic, so a late tick shortens the queue instead of
  /// scheduling a frame in the past. That holds only while the lateness is less
  /// than the queue, so once playback is running the card is asked where its
  /// playhead is first, and the index moved past it if it has been overtaken.
  ///
  /// On failure the frame goes back to the ring: the card took no reference.
  bool scheduleFrame(IDeckLinkMutableVideoFrame* frame) {
    if (playing_) {
      reanchorIfBehind();
    }
    if (output_->ScheduleVideoFrame(frame, schedule_.nextDisplayTime(),
                                    schedule_.frameDuration(),
                                    schedule_.timeScale()) != S_OK) {
      ++scheduleFailures_;
      ring_->release(frame);
      return false;
    }
    schedule_.advance();
    ++framesScheduled_;
    return true;
  }

  /// Issue #13. After a stall longer than the queue — the process stopped,
  /// the machine swapping — the next slot is behind the card's playhead, and
  /// one frame per tick never catches it up. Moving the index to the playhead
  /// plus the preroll restores the depth playback started with; the card holds
  /// its last frame across the slots skipped, as it was doing anyway.
  void reanchorIfBehind() {
    const bool late = counters_->lateSinceCheck.exchange(false);
    BMDTimeValue playhead = 0;
    double speed = 0.0;
    if (output_->GetScheduledStreamTime(schedule_.timeScale(), &playhead, &speed) !=
        S_OK) {
      if (late) {
        counters_->lateSinceCheck.store(true);  // keep it for the next frame
      }
      return;
    }
    const int64_t from = schedule_.nextIndex();
    const int64_t skipped = schedule_.reanchor(playhead, late);
    if (skipped == 0) {
      return;
    }
    ++reanchors_;
    reanchoredFrames_ += skipped;
    // Sound moves by the same span of time as the picture, so the two keep
    // whatever relationship they had before the stall.
    scheduledAudioFrames_ += schedule_.samplePosition(from + skipped, kAudioTimeScale) -
                             schedule_.samplePosition(from, kAudioTimeScale);
    diag::warn("decklink '%s': the card's playhead had overtaken the schedule "
               "(%s); moved %lld frame(s) forward to stay %d ahead",
               name_.c_str(), late ? "after a late or dropped frame" : "after a stall",
               static_cast<long long>(skipped), schedule_.preroll());
  }

  void scheduleAudio(const AudioBlock& audio) {
    const int channels = std::min(audio.channels, audioChannels_);
    audioScratch_.resize(static_cast<size_t>(audio.frames) *
                         static_cast<size_t>(audioChannels_));

    for (int f = 0; f < audio.frames; ++f) {
      for (int c = 0; c < audioChannels_; ++c) {
        float sample = 0.0f;
        if (c < channels) {
          sample = audio.interleaved[static_cast<size_t>(f) * audio.channels + c];
        }
        // Clamp before scaling: a page can and will produce samples outside
        // [-1, 1], and letting those wrap around is the difference between
        // mild distortion and a full-scale click.
        sample = std::clamp(sample, -1.0f, 1.0f);
        audioScratch_[static_cast<size_t>(f) * audioChannels_ + c] =
            static_cast<int32_t>(sample * 2147483647.0f);
      }
    }

    uint32_t written = 0;
    const BMDTimeValue streamTime =
        (scheduledAudioFrames_ * kAudioTimeScale) / audio.sampleRate;
    if (output_->ScheduleAudioSamples(audioScratch_.data(),
                                      static_cast<uint32_t>(audio.frames),
                                      streamTime, kAudioTimeScale,
                                      &written) == S_OK) {
      scheduledAudioFrames_ += written;
    }
  }

  static constexpr BMDTimeScale kAudioTimeScale = 48'000;

  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  IDeckLinkKeyer* keyer_ = nullptr;
  CompletionCallback* callback_ = nullptr;
  std::shared_ptr<PlayoutCounters> counters_ = std::make_shared<PlayoutCounters>();
  std::shared_ptr<FrameRing> ring_;
  PlayoutSchedule schedule_;

  VideoFormat format_;
  BMDDisplayMode displayMode_ = bmdModeHD1080p50;
  BMDTimeValue modeFrameDuration_ = 1;
  BMDTimeScale modeTimeScale_ = 1;
  std::string deviceName_;
  int deviceIndex_;
  int prerollFrames_;
  int ringFrames_;
  int audioChannels_;
  Keying keying_;
  uint8_t keyLevel_;
  bool premultipliedFill_;
  bool videoEnabled_ = false;
  bool audioEnabled_ = false;
  bool playing_ = false;

  int64_t framesScheduled_ = 0;
  int64_t scheduledAudioFrames_ = 0;
  int64_t createFailures_ = 0;
  int64_t scheduleFailures_ = 0;
  int64_t ringMisses_ = 0;  ///< from rings already released
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
  std::vector<AppVideoBuffer*> buffers_;  ///< ours, kept across stop/start
  std::vector<std::pair<IDeckLinkMutableVideoFrame*, AppVideoBuffer*>> frameBuffers_;
  int32_t bufferRowBytes_ = 0;
  size_t bufferBytes_ = 0;
  int64_t bufferAllocations_ = 0;
  int64_t bufferOverlaps_ = 0;  ///< from buffers already released
#endif
  int64_t reanchors_ = 0;
  int64_t reanchoredFrames_ = 0;
  int64_t refusedFrames_ = 0;
  int64_t accessFailures_ = 0;
  std::vector<int32_t> audioScratch_;
};

}  // namespace

std::unique_ptr<IOutput> createDeckLinkOutput(const OutputSpec& spec) {
  return std::make_unique<DeckLinkOutput>(spec);
}

}  // namespace weblinked
