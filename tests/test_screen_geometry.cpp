// The screen output's letterbox arithmetic.
//
// Worth testing on its own because the fit and fill cases are the same two
// branches inverted, and getting one backwards still puts a picture on the
// display — just the wrong size. On a monitor that reads as "close enough"; the
// numbers here are what makes it not.
//
// Header-only, so this needs neither a GPU nor a window nor CEF.

#include "outputs/screen_geometry.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// The scale is in texture coordinates, so what an operator actually sees is
/// its reciprocal: a scale of 2 means the picture occupies half the axis.
constexpr float kEpsilon = 0.0001f;

bool near(float value, float expected) {
  const float difference = value > expected ? value - expected : expected - value;
  return difference < kEpsilon;
}

}  // namespace

WEBLINKED_TEST(screen_scale_is_identity_when_the_aspects_match) {
  // 1920x1080 on a 3840x2160 head is the same shape, so nothing is scaled in
  // either axis regardless of mode — only the resolution differs.
  for (const auto mode : {ScreenScaling::kFit, ScreenScaling::kFill,
                          ScreenScaling::kStretch}) {
    const auto scale = screenScaleFor(1920, 1080, 3840, 2160, mode);
    CHECK(near(scale.x, 1.0f));
    CHECK(near(scale.y, 1.0f));
  }
}

WEBLINKED_TEST(screen_scale_fit_bars_the_axis_that_does_not_fill) {
  // A 16:9 frame on a 4:3 head: fits the width, bars top and bottom. The
  // vertical texture range must therefore exceed 1 so the sampler runs off the
  // picture and the shader paints black there.
  const auto wide = screenScaleFor(1920, 1080, 1024, 768, ScreenScaling::kFit);
  CHECK(near(wide.x, 1.0f));
  CHECK(wide.y > 1.0f);
  // (16/9) / (4/3) = 4/3.
  CHECK(near(wide.y, 4.0f / 3.0f));

  // A 4:3 frame on a 16:9 head: the mirror image, bars left and right.
  const auto tall = screenScaleFor(1024, 768, 1920, 1080, ScreenScaling::kFit);
  CHECK(near(tall.y, 1.0f));
  CHECK(tall.x > 1.0f);
  CHECK(near(tall.x, 4.0f / 3.0f));
}

WEBLINKED_TEST(screen_scale_fill_crops_the_axis_fit_would_have_barred) {
  // Same two shapes as above. Fill is fit inverted: the axis that grew beyond 1
  // must now shrink below it, because the picture overscans instead of barring.
  const auto wide = screenScaleFor(1920, 1080, 1024, 768, ScreenScaling::kFill);
  CHECK(near(wide.y, 1.0f));
  CHECK(wide.x < 1.0f);
  CHECK(near(wide.x, 3.0f / 4.0f));

  const auto tall = screenScaleFor(1024, 768, 1920, 1080, ScreenScaling::kFill);
  CHECK(near(tall.x, 1.0f));
  CHECK(tall.y < 1.0f);
  CHECK(near(tall.y, 3.0f / 4.0f));
}

WEBLINKED_TEST(screen_scale_fit_and_fill_are_reciprocal) {
  // The property that catches a swapped branch: whichever axis fit expands,
  // fill contracts by exactly the same factor, and the other axis stays at 1.
  const auto fit = screenScaleFor(1920, 1080, 2560, 1600, ScreenScaling::kFit);
  const auto fill = screenScaleFor(1920, 1080, 2560, 1600, ScreenScaling::kFill);
  CHECK(near(fit.x * fill.x * fit.y * fill.y, 1.0f));
  CHECK(near(fit.x, 1.0f) != near(fit.y, 1.0f));  // exactly one axis moves
}

WEBLINKED_TEST(screen_scale_stretch_never_bars_or_crops) {
  // Stretch ignores aspect by definition, so it is identity even for a shape
  // as far off as 16:9 into 4:3 — the distortion is the point.
  const auto scale = screenScaleFor(1920, 1080, 1024, 768, ScreenScaling::kStretch);
  CHECK(near(scale.x, 1.0f));
  CHECK(near(scale.y, 1.0f));
}

WEBLINKED_TEST(screen_scale_survives_a_degenerate_size) {
  // A display that reports 0x0 happens when a head is asleep or being
  // reconfigured. Identity is the right answer: a divide by zero here would
  // reach the shader as a NaN and blank the output.
  for (const auto mode : {ScreenScaling::kFit, ScreenScaling::kFill}) {
    for (const auto scale : {screenScaleFor(0, 0, 1920, 1080, mode),
                             screenScaleFor(1920, 1080, 0, 0, mode),
                             screenScaleFor(1920, 0, 0, 1080, mode)}) {
      CHECK(near(scale.x, 1.0f));
      CHECK(near(scale.y, 1.0f));
    }
  }
}

WEBLINKED_TEST(screen_scaling_round_trips_through_its_string_form) {
  CHECK(screenScalingToString(ScreenScaling::kFit) == std::string("fit"));
  CHECK(screenScalingToString(ScreenScaling::kFill) == std::string("fill"));
  CHECK(screenScalingToString(ScreenScaling::kStretch) == std::string("stretch"));
}
