#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/audio_block.h"
#include "core/frame.h"
#include "core/json.h"
#include "core/source_config.h"
#include "core/video_format.h"

namespace weblinked {

/// What to open. `kind` selects the backend; the rest is backend-specific and
/// deliberately loose, because a DeckLink needs a device index and keying mode
/// while NDI needs a source name and nothing else.
struct OutputSpec {
  std::string kind;              ///< "ndi" | "omt" | "decklink" | "aja" | "screen" | "preview"
  std::string name;              ///< NDI/OMT source name, or a label for SDI
  int deviceIndex = 0;           ///< DeckLink/AJA device ordinal
  json::Value options = json::Value::object();
  /// What the page is composited over on the way to this output. Held here
  /// rather than in `options` because no backend acts on it — the engine does,
  /// before the frame ever reaches one — and because changing it must not
  /// reopen the device. See OutputBackground.
  OutputBackground background;

  std::string optionString(const std::string& key, const std::string& fallback = {}) const;
  int optionInt(const std::string& key, int fallback) const;
  bool optionBool(const std::string& key, bool fallback) const;
};

/// A destination for frames.
///
/// Backends are handed frames already in their preferred pixel format, so the
/// engine converts BGRA to UYVY once for all the outputs that want it rather
/// than once each.
///
/// submit() is called from the engine's clock thread. Implementations must not
/// block on it: a backend that stalls holds up every other output. Where a
/// vendor API can block (DeckLink's scheduled playback cannot, but a network
/// send can) the backend either buffers or drops, and says so in status().
class IOutput {
 public:
  virtual ~IOutput() = default;

  virtual std::string kind() const = 0;
  const std::string& name() const { return name_; }

  /// The format submit() expects. Fixed for the lifetime of the output.
  virtual PixelFormat pixelFormat() const = 0;

  /// False for outputs that have nowhere to put audio, so the engine can skip
  /// preparing it when nothing wants it.
  virtual bool wantsAudio() const { return true; }

  /// True when this output carries the alpha channel and expects it *straight*
  /// (unassociated), which NDI, OMT and a DeckLink keyer all do.
  ///
  /// Chromium paints premultiplied, so the engine has to undo that — but only
  /// for outputs that actually key. An opaque output would be paying for a
  /// conversion whose result it then throws away.
  virtual bool wantsStraightAlpha() const { return false; }

  /// Opens the device or sender. On failure returns false and fills `error`
  /// with something an operator can act on.
  virtual bool start(const VideoFormat& format, std::string& error) = 0;

  virtual void stop() = 0;

  /// One frame, plus the audio that belongs with it. `audio` may be invalid
  /// when the page is silent, which is not an error.
  virtual void submit(const VideoFrame& video, const AudioBlock& audio) = 0;

  /// Live counters for the control API. Called from the HTTP thread, so
  /// implementations must guard anything the clock thread also touches.
  virtual json::Value status() const = 0;

  bool isRunning() const { return running_; }

 protected:
  explicit IOutput(std::string name) : name_(std::move(name)) {}

  std::string name_;
  bool running_ = false;
};

/// OutputSpec and OutputConfig carry the same thing for different reasons: one
/// is what the engine opens, the other is what a settings file holds and what
/// `core` can parse without linking a single vendor SDK. These are the only
/// place that knows they are the same fields.
OutputSpec outputSpecFromConfig(const OutputConfig& config);
OutputConfig outputConfigFromSpec(const OutputSpec& spec);

/// Which backends this binary was actually built with. The control UI lists
/// these, so an operator can tell a missing SDK from a missing device.
std::vector<std::string> compiledOutputKinds();

/// Constructs a backend. Returns nullptr with `error` set for an unknown kind
/// or one that was compiled out.
std::unique_ptr<IOutput> createOutput(const OutputSpec& spec, std::string& error);

// Backend constructors, each behind its build flag. Declared here rather than
// in per-backend headers so the factory is the only place that knows the list.
#if defined(WEBLINKED_WITH_NDI)
std::unique_ptr<IOutput> createNdiOutput(const OutputSpec& spec);
#endif
#if defined(WEBLINKED_WITH_OMT)
std::unique_ptr<IOutput> createOmtOutput(const OutputSpec& spec);
#endif
#if defined(WEBLINKED_WITH_DECKLINK)
std::unique_ptr<IOutput> createDeckLinkOutput(const OutputSpec& spec);
#endif
#if defined(WEBLINKED_WITH_AJA)
std::unique_ptr<IOutput> createAjaOutput(const OutputSpec& spec);
#endif
#if defined(WEBLINKED_WITH_SCREEN)
std::unique_ptr<IOutput> createScreenOutput(const OutputSpec& spec);
#endif
std::unique_ptr<IOutput> createPreviewOutput(const OutputSpec& spec);

/// The preview output also serves the control UI's confidence monitor, so the
/// engine needs to reach past IOutput for it.
class PreviewOutput;
PreviewOutput* asPreview(IOutput* output);

}  // namespace weblinked
