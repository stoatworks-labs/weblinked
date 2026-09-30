// The KMS output's turn-and-fit copy.
//
// Every pixel of the source is given its own value, so a wrong rotation, a
// mirrored axis or an off-by-one in the tables shows up as a specific wrong
// pixel rather than as "looks about right" on a monitor. The reference each
// case is checked against is written the obvious way — rotate, then scale — and
// shares no code with the separable tables under test.

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/rotated_blit.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// A source whose every pixel is distinct and never zero, so black bars cannot
/// be mistaken for picture.
std::vector<uint32_t> numbered(int width, int height) {
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      pixels[static_cast<size_t>(y) * width + x] = 0xFF000000u | (y << 12) | (x + 1);
    }
  }
  return pixels;
}

/// Source pixel shown at rotated-picture position (c, r), written directly
/// from the definition of a clockwise turn.
uint32_t turned(const std::vector<uint32_t>& src, int w, int h, BlitRotation rot, int c, int r) {
  int x = c, y = r;
  switch (rot) {
    case BlitRotation::k0: break;
    case BlitRotation::k90: x = r; y = h - 1 - c; break;
    case BlitRotation::k180: x = w - 1 - c; y = h - 1 - r; break;
    case BlitRotation::k270: x = w - 1 - r; y = c; break;
  }
  return src[static_cast<size_t>(y) * w + x];
}

std::vector<uint32_t> run(const std::vector<uint32_t>& src, int sw, int sh, int dw, int dh,
                          BlitRotation rot, BlitFit fit, RotatedBlit* out = nullptr) {
  RotatedBlit blit;
  blit.configure(sw, sh, dw, dh, rot, fit);
  // Pre-filled with a marker so an unwritten pixel is distinguishable from a
  // black bar.
  std::vector<uint32_t> dest(static_cast<size_t>(dw) * dh, 0xDEADBEEFu);
  blit.clearBars(reinterpret_cast<uint8_t*>(dest.data()), dw * 4);
  blit.blit(reinterpret_cast<const uint8_t*>(src.data()), sw * 4,
            reinterpret_cast<uint8_t*>(dest.data()), dw * 4);
  if (out != nullptr) {
    *out = blit;
  }
  return dest;
}

}  // namespace

WEBLINKED_TEST(rotated_blit_parses_only_the_four_right_angles) {
  BlitRotation rotation = BlitRotation::k0;
  CHECK(blitRotationFromString("90", rotation) && rotation == BlitRotation::k90);
  CHECK(blitRotationFromString("-90", rotation) && rotation == BlitRotation::k270);
  CHECK(blitRotationFromString("180", rotation) && rotation == BlitRotation::k180);
  CHECK(blitRotationFromString("0", rotation) && rotation == BlitRotation::k0);
  CHECK(!blitRotationFromString("45", rotation));
  CHECK(!blitRotationFromString("portrait", rotation));
  CHECK(!blitRotationFromString("", rotation));
  CHECK(blitFitFromString("FILL") == BlitFit::kFill);
  CHECK(blitFitFromString("nonsense") == BlitFit::kFit);
}

WEBLINKED_TEST(rotated_blit_turns_one_to_one_exactly_at_every_angle) {
  // Odd, unequal sides: a transposition bug that happens to work on a square
  // or on even sizes is the one this is here to catch.
  const int sw = 7, sh = 5;
  const auto src = numbered(sw, sh);
  for (const auto rot : {BlitRotation::k0, BlitRotation::k90, BlitRotation::k180,
                         BlitRotation::k270}) {
    const bool transposed = rot == BlitRotation::k90 || rot == BlitRotation::k270;
    const int dw = transposed ? sh : sw;
    const int dh = transposed ? sw : sh;
    const auto dest = run(src, sw, sh, dw, dh, rot, BlitFit::kFit);
    for (int y = 0; y < dh; ++y) {
      for (int x = 0; x < dw; ++x) {
        CHECK(dest[static_cast<size_t>(y) * dw + x] == turned(src, sw, sh, rot, x, y));
      }
    }
  }
}

WEBLINKED_TEST(rotated_blit_ninety_puts_the_bottom_left_corner_top_left) {
  // Stated once in plain terms, independently of the reference function: the
  // clockwise sense is the one an operator has to get right when they pick an
  // option for a screen hung on its side.
  const int sw = 4, sh = 2;
  const auto src = numbered(sw, sh);
  const auto dest = run(src, sw, sh, sh, sw, BlitRotation::k90, BlitFit::kFit);
  const uint32_t bottomLeft = src[static_cast<size_t>(sh - 1) * sw + 0];
  const uint32_t topLeft = src[0];
  CHECK(dest[0] == bottomLeft);
  // ...and the source's top-left ends up top-right.
  CHECK(dest[static_cast<size_t>(sh - 1)] == topLeft);
}

WEBLINKED_TEST(rotated_blit_portrait_page_on_a_landscape_head_is_barred_left_and_right) {
  // 90x160 turned 0 onto 320x180: fits the height, 90*180/160 = 101.25 wide,
  // rounded to 101, centred. Everything outside it is black and everything
  // inside it is picture.
  const int sw = 90, sh = 160, dw = 320, dh = 180;
  const auto src = numbered(sw, sh);
  RotatedBlit blit;
  const auto dest = run(src, sw, sh, dw, dh, BlitRotation::k0, BlitFit::kFit, &blit);
  CHECK(blit.placement().height == 180);
  CHECK(blit.placement().width == 101);
  CHECK(blit.placement().x == (320 - 101) / 2);
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dw; ++x) {
      const uint32_t pixel = dest[static_cast<size_t>(y) * dw + x];
      const bool inside = x >= blit.placement().x &&
                          x < blit.placement().x + blit.placement().width;
      CHECK(pixel != 0xDEADBEEFu);
      CHECK(inside ? pixel != 0 : pixel == 0);
    }
  }
}

WEBLINKED_TEST(rotated_blit_a_turned_portrait_page_fills_a_landscape_head) {
  // The signage case: a 1080x1920-shaped page turned onto a 1920x1080-shaped
  // screen. Scaled down 10x here; there must be no bars at all and the corners
  // must be the right source corners.
  const int sw = 108, sh = 192, dw = 192, dh = 108;
  const auto src = numbered(sw, sh);
  for (const auto rot : {BlitRotation::k90, BlitRotation::k270}) {
    RotatedBlit blit;
    const auto dest = run(src, sw, sh, dw, dh, rot, BlitFit::kFit, &blit);
    CHECK(blit.placement().x == 0 && blit.placement().y == 0);
    CHECK(blit.placement().width == dw && blit.placement().height == dh);
    for (const auto pixel : dest) {
      CHECK(pixel != 0 && pixel != 0xDEADBEEFu);
    }
    CHECK(dest[0] == turned(src, sw, sh, rot, 0, 0));
    CHECK(dest.back() == turned(src, sw, sh, rot, dw - 1, dh - 1));
  }
}

WEBLINKED_TEST(rotated_blit_scales_by_pixel_centres) {
  // A 2:1 downscale must take every other pixel starting from the centre of
  // each pair, not always the left one: 8 wide onto 4 samples columns 0,2,4,6
  // only if it is using the left edge. Centres of [0,2) is 1, so 1,3,5,7.
  const int sw = 8, sh = 2;
  const auto src = numbered(sw, sh);
  const auto dest = run(src, sw, sh, 4, 1, BlitRotation::k0, BlitFit::kStretch);
  CHECK(dest[0] == src[static_cast<size_t>(1) * sw + 1]);
  CHECK(dest[3] == src[static_cast<size_t>(1) * sw + 7]);
}

WEBLINKED_TEST(rotated_blit_fill_crops_instead_of_barring) {
  // 16:9 onto 4:3 with fill: height fills, width overscans; no black anywhere.
  const int sw = 160, sh = 90, dw = 40, dh = 30;
  const auto src = numbered(sw, sh);
  RotatedBlit blit;
  const auto dest = run(src, sw, sh, dw, dh, BlitRotation::k0, BlitFit::kFill, &blit);
  CHECK(blit.placement().height == dh);
  CHECK(blit.placement().width > dw);
  CHECK(blit.placement().x < 0);
  for (const auto pixel : dest) {
    CHECK(pixel != 0 && pixel != 0xDEADBEEFu);
  }
}

WEBLINKED_TEST(rotated_blit_respects_row_padding_on_both_sides) {
  // KMS dumb buffers have a pitch the driver chooses, often wider than the
  // mode, and CEF frames can be padded too. Neither padding may be read as
  // picture or written.
  const int sw = 5, sh = 3, sp = 8;   // source pitch in pixels
  const int dw = 3, dh = 5, dp = 6;   // destination pitch in pixels
  std::vector<uint32_t> src(static_cast<size_t>(sp) * sh, 0x11111111u);
  const auto packed = numbered(sw, sh);
  for (int y = 0; y < sh; ++y) {
    std::memcpy(&src[static_cast<size_t>(y) * sp], &packed[static_cast<size_t>(y) * sw], sw * 4);
  }
  std::vector<uint32_t> dest(static_cast<size_t>(dp) * dh, 0xDEADBEEFu);
  RotatedBlit blit;
  blit.configure(sw, sh, dw, dh, BlitRotation::k270, BlitFit::kFit);
  blit.clearBars(reinterpret_cast<uint8_t*>(dest.data()), dp * 4);
  blit.blit(reinterpret_cast<const uint8_t*>(src.data()), sp * 4,
            reinterpret_cast<uint8_t*>(dest.data()), dp * 4);
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dp; ++x) {
      const uint32_t pixel = dest[static_cast<size_t>(y) * dp + x];
      if (x < dw) {
        CHECK(pixel == turned(packed, sw, sh, BlitRotation::k270, x, y));
      } else {
        CHECK(pixel == 0xDEADBEEFu);  // padding untouched
      }
    }
  }
}

WEBLINKED_TEST(rotated_blit_degenerate_sizes_draw_nothing_and_do_not_crash) {
  RotatedBlit blit;
  blit.configure(0, 0, 10, 10, BlitRotation::k90, BlitFit::kFit);
  std::vector<uint32_t> dest(100, 0xDEADBEEFu);
  blit.clearBars(reinterpret_cast<uint8_t*>(dest.data()), 40);
  blit.blit(nullptr, 0, reinterpret_cast<uint8_t*>(dest.data()), 40);
  for (const auto pixel : dest) {
    CHECK(pixel == 0);
  }
}
