#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weblinked {

/// How far the picture is turned, clockwise, on its way to a display.
///
/// Clockwise *of the picture*, not of the monitor: a screen hung with its top
/// edge on the right (turned 90° clockwise) needs the picture turned 270° to
/// read upright, and one turned anticlockwise needs 90°.
enum class BlitRotation { k0 = 0, k90 = 90, k180 = 180, k270 = 270 };

/// How a picture whose shape differs from the display is fitted to it. The
/// same three modes, with the same meanings, as the screen output's.
enum class BlitFit { kFit, kFill, kStretch };

/// Accepts "0", "90", "180", "270" (and "-90" as 270). Returns false for
/// anything else, so a typo is refused rather than silently drawn upright.
bool blitRotationFromString(const std::string& text, BlitRotation& rotation);
const char* blitRotationToString(BlitRotation rotation);
/// Unknown spellings fall back to fit, as screenScalingFromString does.
BlitFit blitFitFromString(const std::string& text);
const char* blitFitToString(BlitFit fit);

/// Where a rotated, fitted picture lands on a display. May extend past the
/// display on either axis in fill mode; the blit clips it.
struct BlitRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

/// Copies a 32-bit BGRA picture onto a 32-bit display buffer, turned and
/// fitted, with nearest-neighbour sampling.
///
/// Built for a CPU-only display path — a KMS dumb buffer on a board with no
/// usable GPU — so the per-frame work is one read and one write per displayed
/// pixel and nothing else. Everything that depends only on the geometry is
/// worked out once, in configure():
///
///   every destination pixel's source address is xOffset[x] + yOffset[y],
///
/// for all four rotations. That separability is what keeps the inner loop to a
/// table lookup and a 32-bit copy, and it is why a rotation costs no more
/// arithmetic than an upright copy — only worse locality, which the tiled loop
/// in blit() is there to recover.
///
/// XRGB8888 and BGRA8888 are the same four bytes in memory on a little-endian
/// machine, so no channel is touched. Alpha is carried into the X byte and
/// ignored by the display; a transparent page therefore shows over black, as
/// the premultiplied paint already assumes.
class RotatedBlit {
 public:
  /// Recomputes the tables. Cheap enough to call on every format change, too
  /// expensive to call per frame.
  void configure(int sourceWidth, int sourceHeight, int destWidth, int destHeight,
                 BlitRotation rotation, BlitFit fit);

  /// The picture's placement on the display, before clipping.
  const BlitRect& placement() const { return placement_; }

  /// The displayed raster once turned: the source's own for 0/180, transposed
  /// for 90/270.
  int rotatedWidth() const { return rotatedWidth_; }
  int rotatedHeight() const { return rotatedHeight_; }

  /// Fills every destination pixel outside the placement with black. Needed
  /// once per destination buffer, not per frame: blit() never writes there.
  void clearBars(uint8_t* dest, int destRowBytes) const;

  /// One frame. `source` must be the size configure() was given; the caller
  /// guarantees it (the engine drops stale-raster frames before any output
  /// sees them).
  void blit(const uint8_t* source, int sourceRowBytes, uint8_t* dest,
            int destRowBytes) const;

 private:
  int sourceWidth_ = 0;
  int sourceHeight_ = 0;
  int destWidth_ = 0;
  int destHeight_ = 0;
  int rotatedWidth_ = 0;
  int rotatedHeight_ = 0;
  BlitRotation rotation_ = BlitRotation::k0;
  BlitRect placement_;
  // The visible part of the placement, clipped to the display.
  int x0_ = 0, x1_ = 0, y0_ = 0, y1_ = 0;
  // Per destination column / row: which rotated-picture column / row it shows.
  std::vector<int> columnOf_;
  std::vector<int> rowOf_;
  // True when a straight row copy is exact: upright and 1:1.
  bool identity_ = false;
};

}  // namespace weblinked
