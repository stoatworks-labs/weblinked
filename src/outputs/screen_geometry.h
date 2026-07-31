#pragma once

#include "outputs/screen_window.h"

namespace weblinked {

/// The texture-coordinate scale that fits a frame to a display.
///
/// Header-only and free of any platform type on purpose: all three screen
/// backends need exactly this arithmetic, and it is the part of them that is
/// easy to get subtly wrong — the two cases invert, and an inverted one still
/// produces a picture, just the wrong size, which is the kind of bug that
/// survives a casual look at a monitor. Kept here it is written once and
/// covered by tests/test_screen_geometry.cpp without a GPU in sight.
///
/// The scale is applied to *texture* coordinates, not to the picture, so the
/// senses run backwards from the intuitive reading: a value above 1 samples
/// outside the frame and produces bars, and one below 1 crops.
struct ScreenScale {
  float x = 1.0f;
  float y = 1.0f;
};

inline ScreenScale screenScaleFor(int frameWidth, int frameHeight,
                                  int displayWidth, int displayHeight,
                                  ScreenScaling scaling) {
  ScreenScale scale;
  if (frameWidth <= 0 || frameHeight <= 0 || displayWidth <= 0 ||
      displayHeight <= 0 || scaling == ScreenScaling::kStretch) {
    // Stretch is 1:1 in both axes by definition. A degenerate size falls back
    // to the same thing rather than dividing by zero.
    return scale;
  }

  const double frameAspect = static_cast<double>(frameWidth) / frameHeight;
  const double displayAspect = static_cast<double>(displayWidth) / displayHeight;
  const double ratio = frameAspect / displayAspect;
  const bool frameIsWider = ratio > 1.0;

  if (scaling == ScreenScaling::kFit) {
    if (frameIsWider) {
      scale.y = static_cast<float>(ratio);  // bars top and bottom
    } else {
      scale.x = static_cast<float>(1.0 / ratio);  // bars left and right
    }
  } else {  // kFill — the same two cases, inverted.
    if (frameIsWider) {
      scale.x = static_cast<float>(1.0 / ratio);  // crop left and right
    } else {
      scale.y = static_cast<float>(ratio);  // crop top and bottom
    }
  }
  return scale;
}

}  // namespace weblinked
