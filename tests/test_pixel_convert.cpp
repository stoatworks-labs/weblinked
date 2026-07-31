// Tests for the BGRA -> UYVY converter.
//
// The reference below is an independent floating-point implementation written
// straight from the BT.601/BT.709 definitions. Comparing the shipped integer
// path against the same integer coefficients would only prove the code equals
// itself; comparing against the float definition is what catches a wrong
// coefficient, a swapped Cb/Cr, or a missing studio-swing offset.
//
// This matters more than it looks. A previous project in this fleet shipped a
// broadband saturator as a "harmonic EQ" through four releases because a
// single-tone test could not tell the difference. A single-colour test cannot
// tell a right matrix from a wrong one either — so the reference sweep below
// covers a spread of colours, not just black and white.

#include <cstdint>
#include <vector>

#include "core/pixel_convert.h"
#include "test_support.h"

using namespace weblinked;

namespace {

struct Ycbcr {
  double y, cb, cr;
};

/// Independent reference: BT.601/709 studio swing, from the definitions.
Ycbcr reference(int r, int g, int b, ColourMatrix matrix) {
  const double kr = matrix == ColourMatrix::kBt709 ? 0.2126 : 0.299;
  const double kb = matrix == ColourMatrix::kBt709 ? 0.0722 : 0.114;
  const double kg = 1.0 - kr - kb;

  const double rn = r / 255.0, gn = g / 255.0, bn = b / 255.0;
  const double yn = kr * rn + kg * gn + kb * bn;

  Ycbcr out;
  out.y = 16.0 + 219.0 * yn;
  out.cb = 128.0 + 224.0 * ((bn - yn) / (2.0 * (1.0 - kb)));
  out.cr = 128.0 + 224.0 * ((rn - yn) / (2.0 * (1.0 - kr)));
  return out;
}

/// One 2x1 BGRA pixel pair through the converter, giving the 4 UYVY bytes.
struct Uyvy {
  uint8_t cb, y0, cr, y1;
};

Uyvy convertPair(int r0, int g0, int b0, int r1, int g1, int b1,
                 ColourMatrix matrix) {
  uint8_t src[8] = {static_cast<uint8_t>(b0), static_cast<uint8_t>(g0),
                    static_cast<uint8_t>(r0), 255,
                    static_cast<uint8_t>(b1), static_cast<uint8_t>(g1),
                    static_cast<uint8_t>(r1), 255};
  uint8_t dst[4] = {};
  bgraToUyvy(src, 8, dst, 4, 2, 1, matrix);
  return {dst[0], dst[1], dst[2], dst[3]};
}

Uyvy convertSolid(int r, int g, int b, ColourMatrix matrix) {
  return convertPair(r, g, b, r, g, b, matrix);
}

}  // namespace

WEBLINKED_TEST(uyvy_black_and_white_hit_studio_swing_limits) {
  for (auto matrix : {ColourMatrix::kBt601, ColourMatrix::kBt709}) {
    const Uyvy black = convertSolid(0, 0, 0, matrix);
    CHECK_EQ(static_cast<int>(black.y0), 16);
    CHECK_EQ(static_cast<int>(black.y1), 16);

    const Uyvy white = convertSolid(255, 255, 255, matrix);
    CHECK_EQ(static_cast<int>(white.y0), 235);
    CHECK_EQ(static_cast<int>(white.y1), 235);
  }
}

WEBLINKED_TEST(uyvy_neutral_grey_is_exactly_128_chroma) {
  // The reason the coefficient rows are rounded to sum to zero. Any tint here
  // would be visible on a ramp or a grey background, and would be blamed on
  // everything except the converter.
  for (auto matrix : {ColourMatrix::kBt601, ColourMatrix::kBt709}) {
    for (int level : {0, 1, 32, 64, 127, 128, 192, 254, 255}) {
      const Uyvy grey = convertSolid(level, level, level, matrix);
      CHECK_EQ(static_cast<int>(grey.cb), 128);
      CHECK_EQ(static_cast<int>(grey.cr), 128);
    }
  }
}

WEBLINKED_TEST(uyvy_matches_float_reference_across_colour_space) {
  // A deterministic sweep rather than random values, so a failure is
  // reproducible without a seed.
  for (auto matrix : {ColourMatrix::kBt601, ColourMatrix::kBt709}) {
    for (int r = 0; r <= 255; r += 17) {
      for (int g = 0; g <= 255; g += 17) {
        for (int b = 0; b <= 255; b += 17) {
          const Uyvy actual = convertSolid(r, g, b, matrix);
          const Ycbcr expected = reference(r, g, b, matrix);
          // One LSB of tolerance: the shipped path is 16.16 fixed point.
          CHECK_NEAR(actual.y0, expected.y, 1.0);
          CHECK_NEAR(actual.cb, expected.cb, 1.0);
          CHECK_NEAR(actual.cr, expected.cr, 1.0);
        }
      }
    }
  }
}

WEBLINKED_TEST(uyvy_primaries_land_on_known_values) {
  // Cross-checks against values quotable from the standards, so the reference
  // implementation above cannot be wrong in the same direction as the code.
  const Uyvy red709 = convertSolid(255, 0, 0, ColourMatrix::kBt709);
  CHECK_EQ(static_cast<int>(red709.y0), 63);   // 16 + 219*0.2126
  CHECK_EQ(static_cast<int>(red709.cr), 240);  // Cr at maximum excursion

  const Uyvy blue601 = convertSolid(0, 0, 255, ColourMatrix::kBt601);
  CHECK_EQ(static_cast<int>(blue601.y0), 41);  // 16 + 219*0.114
  CHECK_EQ(static_cast<int>(blue601.cb), 240);

  const Uyvy green709 = convertSolid(0, 255, 0, ColourMatrix::kBt709);
  CHECK_EQ(static_cast<int>(green709.y0), 173);  // 16 + 219*0.7152 = 172.6
}

WEBLINKED_TEST(uyvy_byte_order_is_cb_y0_cr_y1) {
  // Two different luma values in one pair, so a swapped Y0/Y1 cannot pass.
  const Uyvy pair = convertPair(0, 0, 0, 255, 255, 255, ColourMatrix::kBt709);
  CHECK_EQ(static_cast<int>(pair.y0), 16);
  CHECK_EQ(static_cast<int>(pair.y1), 235);
  // Averaged black and white is still neutral.
  CHECK_EQ(static_cast<int>(pair.cb), 128);
  CHECK_EQ(static_cast<int>(pair.cr), 128);
}

WEBLINKED_TEST(uyvy_chroma_is_averaged_across_the_pair) {
  // Red next to black: chroma should sit halfway between red's Cr and neutral,
  // which is what distinguishes averaging from point-sampling the first pixel.
  const Uyvy pair = convertPair(255, 0, 0, 0, 0, 0, ColourMatrix::kBt709);
  const Ycbcr halfRed = reference(128, 0, 0, ColourMatrix::kBt709);
  CHECK_NEAR(pair.cr, halfRed.cr, 1.5);
  CHECK(pair.cr > 128 && pair.cr < 240);
}

WEBLINKED_TEST(uyvy_respects_stride_padding) {
  // 2 pixels wide, 2 rows, with deliberate padding on both sides. Ignoring
  // stride would read the padding as picture.
  const int srcStride = 8 + 12;
  const int dstStride = 4 + 6;
  std::vector<uint8_t> src(static_cast<size_t>(srcStride) * 2, 0xAA);
  std::vector<uint8_t> dst(static_cast<size_t>(dstStride) * 2, 0x00);

  for (int y = 0; y < 2; ++y) {
    uint8_t* row = src.data() + static_cast<size_t>(y) * srcStride;
    for (int x = 0; x < 2; ++x) {
      row[x * 4 + 0] = 255;  // B
      row[x * 4 + 1] = 255;  // G
      row[x * 4 + 2] = 255;  // R
      row[x * 4 + 3] = 255;
    }
  }

  bgraToUyvy(src.data(), srcStride, dst.data(), dstStride, 2, 2,
             ColourMatrix::kBt709);

  for (int y = 0; y < 2; ++y) {
    const uint8_t* row = dst.data() + static_cast<size_t>(y) * dstStride;
    CHECK_EQ(static_cast<int>(row[1]), 235);
    CHECK_EQ(static_cast<int>(row[3]), 235);
    // Padding beyond the active picture must be untouched.
    CHECK_EQ(static_cast<int>(row[4]), 0);
  }
}

WEBLINKED_TEST(auto_matrix_switches_at_720_lines) {
  CHECK(resolveMatrix(ColourMatrix::kAuto, 576) == ColourMatrix::kBt601);
  CHECK(resolveMatrix(ColourMatrix::kAuto, 486) == ColourMatrix::kBt601);
  CHECK(resolveMatrix(ColourMatrix::kAuto, 720) == ColourMatrix::kBt709);
  CHECK(resolveMatrix(ColourMatrix::kAuto, 1080) == ColourMatrix::kBt709);
  CHECK(resolveMatrix(ColourMatrix::kBt601, 1080) == ColourMatrix::kBt601);
}

WEBLINKED_TEST(copy_bgra_can_flip_vertically) {
  const int width = 2, height = 3, stride = width * 4;
  std::vector<uint8_t> src(static_cast<size_t>(stride) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width * 4; ++x) {
      src[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>(y + 1);
    }
  }

  std::vector<uint8_t> straight(src.size(), 0);
  copyBgra(src.data(), stride, straight.data(), stride, width, height, false);
  CHECK_EQ(static_cast<int>(straight[0]), 1);
  CHECK_EQ(static_cast<int>(straight[static_cast<size_t>(2) * stride]), 3);

  std::vector<uint8_t> flipped(src.size(), 0);
  copyBgra(src.data(), stride, flipped.data(), stride, width, height, true);
  CHECK_EQ(static_cast<int>(flipped[0]), 3);
  CHECK_EQ(static_cast<int>(flipped[static_cast<size_t>(2) * stride]), 1);
}

WEBLINKED_TEST(fill_black_produces_legal_black) {
  std::vector<uint8_t> bgra(4 * 4 * 2);
  fillBlackBgra(bgra.data(), 4 * 4, 4, 2);
  CHECK_EQ(static_cast<int>(bgra[0]), 0);
  CHECK_EQ(static_cast<int>(bgra[3]), 255);  // opaque, not clear

  std::vector<uint8_t> uyvy(4 * 2 * 2);
  fillBlackUyvy(uyvy.data(), 4 * 2, 4, 2);
  CHECK_EQ(static_cast<int>(uyvy[0]), 128);  // Cb
  CHECK_EQ(static_cast<int>(uyvy[1]), 16);   // Y
  CHECK_EQ(static_cast<int>(uyvy[2]), 128);  // Cr
  CHECK_EQ(static_cast<int>(uyvy[3]), 16);
}

WEBLINKED_TEST(uyvy_writes_nothing_outside_the_declared_extent) {
  // Regression guard for a real bug. During a runtime format change the engine
  // briefly held a 1920x1080 frame while its UYVY pool had already been rebuilt
  // at 1280x720, and converted one into the other — writing about four megabytes
  // past the end of the destination. It did not fault where it happened; it
  // surfaced frames later as a jump through a clobbered vtable.
  //
  // The engine now refuses to convert mismatched rasters. This test pins the
  // other half of that contract: given honest dimensions, the converter must
  // stay strictly inside the destination it was handed.
  const int width = 64, height = 8;
  const int dstStride = width * 2;
  const size_t active = static_cast<size_t>(dstStride) * height;
  const size_t guard = 4096;

  std::vector<uint8_t> src(static_cast<size_t>(width) * 4 * height, 0x7f);
  std::vector<uint8_t> dst(active + guard, 0xCD);

  bgraToUyvy(src.data(), width * 4, dst.data(), dstStride, width, height,
             ColourMatrix::kBt709);

  for (size_t i = active; i < dst.size(); ++i) {
    if (dst[i] != 0xCD) {
      CHECK(false);  // wrote past the end of the destination
      break;
    }
  }
  CHECK(dst[active - 1] != 0xCD);  // and it really did fill the last active byte
}

WEBLINKED_TEST(unpremultiply_recovers_the_original_colour) {
  // Chromium composites premultiplied: 50%-opaque pure green arrives as
  // (0,128,0,128), not (0,255,0,128). NDI, OMT and a DeckLink keyer all expect
  // straight alpha, so sending Chromium's buffer unchanged makes every
  // partially transparent pixel too dark — soft edges, shadows and fades render
  // muddy. Measured over NDI before the fix: 50% green came out at Y=95 against
  // a correct 173.
  struct Case {
    uint8_t b, g, r, a;        // premultiplied input
    uint8_t wantB, wantG, wantR;  // straight output
  };
  const Case cases[] = {
      {0, 128, 0, 128, 0, 255, 0},      // half-opaque green
      {0, 0, 64, 64, 0, 0, 255},        // quarter-opaque red
      {10, 20, 30, 255, 10, 20, 30},    // opaque passes through untouched
      {0, 0, 0, 0, 0, 0, 0},            // fully transparent has no colour left
      {64, 64, 64, 128, 128, 128, 128}, // mid grey
  };

  for (const auto& test : cases) {
    uint8_t src[4] = {test.b, test.g, test.r, test.a};
    uint8_t dst[4] = {};
    unpremultiplyBgra(src, 4, dst, 4, 1, 1);
    CHECK_EQ(static_cast<int>(dst[0]), static_cast<int>(test.wantB));
    CHECK_EQ(static_cast<int>(dst[1]), static_cast<int>(test.wantG));
    CHECK_EQ(static_cast<int>(dst[2]), static_cast<int>(test.wantR));
    // Alpha itself must survive untouched, or the key is wrong.
    CHECK_EQ(static_cast<int>(dst[3]), static_cast<int>(test.a));
  }
}

WEBLINKED_TEST(unpremultiply_clamps_rather_than_wrapping) {
  // A colour channel above its own alpha is not physically meaningful, but it
  // does occur — rounding in the compositor, or a source that was never really
  // premultiplied. Dividing it up must saturate, not wrap round to near zero,
  // which would show as bright confetti on a key edge.
  uint8_t src[4] = {200, 200, 200, 100};
  uint8_t dst[4] = {};
  unpremultiplyBgra(src, 4, dst, 4, 1, 1);
  CHECK_EQ(static_cast<int>(dst[0]), 255);
  CHECK_EQ(static_cast<int>(dst[1]), 255);
  CHECK_EQ(static_cast<int>(dst[2]), 255);
  CHECK_EQ(static_cast<int>(dst[3]), 100);
}

WEBLINKED_TEST(unpremultiply_stays_inside_its_destination) {
  // Same guard-band contract as the UYVY converter.
  const int width = 32, height = 4;
  const int stride = width * 4;
  const size_t active = static_cast<size_t>(stride) * height;
  std::vector<uint8_t> src(active, 0x40);
  std::vector<uint8_t> dst(active + 2048, 0xCD);

  unpremultiplyBgra(src.data(), stride, dst.data(), stride, width, height);

  for (size_t i = active; i < dst.size(); ++i) {
    if (dst[i] != 0xCD) {
      CHECK(false);
      break;
    }
  }
  CHECK(dst[active - 1] != 0xCD);
}

WEBLINKED_TEST(downscale_preserves_a_solid_colour) {
  const int width = 8, height = 8, stride = width * 4;
  std::vector<uint8_t> src(static_cast<size_t>(stride) * height);
  for (size_t i = 0; i < src.size(); i += 4) {
    src[i + 0] = 10;   // B
    src[i + 1] = 20;   // G
    src[i + 2] = 30;   // R
    src[i + 3] = 255;
  }

  const int factor = 4;
  const int dstWidth = width / factor;
  std::vector<uint8_t> dst(static_cast<size_t>(dstWidth) * 4 * (height / factor));
  downscaleBgra(src.data(), stride, width, height, dst.data(), dstWidth * 4, factor);

  for (size_t i = 0; i < dst.size(); i += 4) {
    CHECK_EQ(static_cast<int>(dst[i + 0]), 10);
    CHECK_EQ(static_cast<int>(dst[i + 1]), 20);
    CHECK_EQ(static_cast<int>(dst[i + 2]), 30);
  }
}
