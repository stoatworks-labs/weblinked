#pragma once

#include <cstdint>

namespace weblinked {

/// One pointer or keyboard event on its way to the offscreen page.
///
/// Pointer positions are **normalised** (0..1 across the raster) rather than in
/// pixels. The control page's preview is a downscaled canvas whose size depends
/// on the browser window, the preview factor and the output raster, and only the
/// page knows its own canvas size — so it normalises, and the engine scales back
/// up to whatever the current raster happens to be. That also means a format
/// change mid-drag cannot send a click off the edge of the page.
///
/// Deliberately free of CEF types: the control API constructs these from JSON
/// without needing to know Chromium's enums.
struct InputEvent {
  enum class Type {
    kMove,
    kButton,
    kWheel,
    kKey,
    kFocus,
  };

  enum class Button {
    kLeft = 0,
    kMiddle = 1,
    kRight = 2,
  };

  /// Matches cef_key_event_type_t, translated in browser_source.cpp.
  enum class KeyAction {
    kRawKeyDown = 0,
    kKeyDown = 1,
    kKeyUp = 2,
    kChar = 3,
  };

  Type type = Type::kMove;

  double nx = 0.0;  ///< 0..1 across the raster
  double ny = 0.0;  ///< 0..1 down the raster

  Button button = Button::kLeft;
  bool down = false;
  int clickCount = 1;
  bool leaving = false;  ///< pointer left the preview

  int deltaX = 0;
  int deltaY = 0;

  KeyAction keyAction = KeyAction::kRawKeyDown;
  /// A Windows virtual-key code. Browsers' KeyboardEvent.keyCode is close
  /// enough to one that Chromium accepts it, which is what the control page
  /// sends.
  int keyCode = 0;
  /// UTF-16 code unit for a character-producing key; 0 otherwise.
  int character = 0;

  bool focused = false;

  /// CEF event flags (shift, control, alt, command, mouse buttons).
  uint32_t modifiers = 0;
};

}  // namespace weblinked
