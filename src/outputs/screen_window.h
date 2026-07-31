#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/video_format.h"

namespace weblinked {

/// One GPU-attached display, as the control page offers it.
struct DisplayInfo {
  int index = 0;         ///< What --screen=<n> and options.display take.
  std::string name;      ///< "Built-in Retina Display", "DELL U2724D"
  int width = 0;         ///< Pixels, not points: what the output actually fills.
  int height = 0;
  double refreshHz = 0;  ///< 0 when the platform will not say.
  bool primary = false;
};

/// How a frame is fitted to a display whose shape does not match it.
enum class ScreenScaling {
  kFit,      ///< Whole frame visible, bars where the aspects differ. Default.
  kFill,     ///< Fills the display, overscanning the long axis.
  kStretch,  ///< Ignores aspect entirely.
};

/// Inline rather than in screen_output.cpp so the tests can reach them: that
/// translation unit lives in weblinked_engine, which drags in CEF, and
/// tests/ links weblinked_core only.
inline ScreenScaling screenScalingFromString(const std::string& text) {
  std::string lower;
  lower.reserve(text.size());
  std::transform(text.begin(), text.end(), std::back_inserter(lower),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "fill") return ScreenScaling::kFill;
  if (lower == "stretch") return ScreenScaling::kStretch;
  // Unknown spellings fall back to fit rather than failing the output. An
  // operator who mistypes a scaling mode should get a picture with bars, not a
  // display that stays black.
  return ScreenScaling::kFit;
}

inline const char* screenScalingToString(ScreenScaling scaling) {
  switch (scaling) {
    case ScreenScaling::kFill:
      return "fill";
    case ScreenScaling::kStretch:
      return "stretch";
    case ScreenScaling::kFit:
      break;
  }
  return "fit";
}

/// A borderless fullscreen window on one display, and the GPU path that fills
/// it.
///
/// Three implementations, one per platform, chosen at build time — there is no
/// runtime dispatch because a binary only ever has one of them.
///
/// The threading contract is the whole reason this interface exists, so it is
/// stated rather than implied:
///
///   - Every method is callable from any thread. This matters because start()
///     arrives on the main thread at boot but on the HTTP thread when an
///     operator adds an output from the control page, and a window cannot be
///     created from just anywhere. **Implementations marshal internally**, each
///     to whatever its platform demands — macOS to the main thread, where
///     AppKit insists windows live; Windows and Linux to a thread of their own
///     carrying the event pump that belongs to the window.
///   - present() is called from the engine's clock thread and must not block.
///     It copies the frame into GPU-visible memory and returns; nothing about
///     it waits for a draw.
///   - The actual draw happens on the platform's own display-refresh callback,
///     which is a third thread. Implementations own that synchronisation.
///
/// Presentation is paced by the *display*, not by the engine clock. A 50 Hz
/// page on a 60 Hz head repeats frames rather than tearing, which is what a
/// projector wants; presentedCount() minus submittedCount() is therefore
/// expected to be non-zero and is not an error.
class ScreenWindow {
 public:
  virtual ~ScreenWindow() = default;

  /// Opens a fullscreen window on `display`. Returns false with `error` set to
  /// something an operator can act on — a display index that is not there is
  /// the common case, so it names how many there are.
  virtual bool open(const VideoFormat& format, int display, ScreenScaling scaling,
                    std::string& error) = 0;

  virtual void close() = 0;

  /// Hands over one frame, which must be kBGRA. Copies; the reference does not
  /// outlive the call.
  virtual void present(const VideoFrame& frame) = 0;

  /// Frames actually put on the glass. Compare against the engine's tick count
  /// to see repetition on a faster head, or drops on a slower one.
  virtual int64_t presentedCount() const = 0;

  /// Frames handed to present() that were overwritten before a refresh could
  /// show them. Normal when the page runs faster than the display.
  virtual int64_t droppedCount() const = 0;

  /// A short description of the GPU path actually in use, for the control
  /// page's status — "Metal, Apple M4 Max" and so on.
  virtual std::string describe() const = 0;
};

/// Constructs the platform's implementation. Never null.
std::unique_ptr<ScreenWindow> createScreenWindow();

/// Every display the platform reports, in the order --screen indexes them.
///
/// Safe to call before any window exists, and from any thread: the control API
/// calls it from the HTTP thread to populate the settings page.
std::vector<DisplayInfo> enumerateDisplays();

}  // namespace weblinked
