#include "core/pixel_convert.h"

#include <algorithm>
#include <cstring>

namespace weblinked {
namespace {

// Fixed-point RGB -> Y'CbCr, studio swing, 16.16 coefficients.
//
// Y  = Kr*R + Kg*G + Kb*B, scaled to [16,235]
// Cb = (B - Y) / (2 * (1 - Kb)), centred on 128, scaled to [16,240]
// Cr = (R - Y) / (2 * (1 - Kr)), likewise
//
// Integer coefficients are the standard ones rather than derived at runtime, so
// the output is bit-stable across compilers and platforms — which is what makes
// the converter testable at all.
struct Coefficients {
  int32_t yr, yg, yb, yOffset;
  int32_t ur, ug, ub;
  int32_t vr, vg, vb;
};

// Values are the conventional 8-bit studio-swing integer coefficients scaled by
// 2^16 (see e.g. BT.601 §2.5.1 and BT.709 §3, expressed for 219/224 excursion).
//
// Each chroma row is rounded so that it sums to exactly zero, even where that
// means taking the second-nearest integer for one coefficient. That property is
// worth one part in 65536 of accuracy: it guarantees any neutral input
// (R == G == B) produces Cb = Cr = 128 exactly, so grey never picks up a tint.
// tests/test_pixel_convert.cpp asserts it.
constexpr Coefficients kBt601 = {
    16829, 33039, 6416, 16 << 16,   // Y  = 0.2568 R + 0.5041 G + 0.0979 B + 16
    -9714, -19070, 28784,           // Cb = -0.1482 R - 0.2910 G + 0.4392 B
    28784, -24103, -4681,           // Cr =  0.4392 R - 0.3678 G - 0.0714 B
};

constexpr Coefficients kBt709 = {
    11966, 40254, 4064, 16 << 16,   // Y  = 0.1826 R + 0.6142 G + 0.0620 B + 16
    -6596, -22188, 28784,           // Cb = -0.1006 R - 0.3386 G + 0.4392 B
    28784, -26145, -2639,           // Cr =  0.4392 R - 0.3989 G - 0.0403 B
};

inline uint8_t clampByte(int32_t value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// Rounds a 16.16 fixed-point accumulator to the nearest integer.
inline int32_t round16(int32_t fixed) {
  return (fixed + (1 << 15)) >> 16;
}

}  // namespace

ColourMatrix resolveMatrix(ColourMatrix matrix, int height) {
  if (matrix != ColourMatrix::kAuto) {
    return matrix;
  }
  return height >= 720 ? ColourMatrix::kBt709 : ColourMatrix::kBt601;
}

void bgraToUyvy(const uint8_t* src, int srcStride,
                uint8_t* dst, int dstStride,
                int width, int height,
                ColourMatrix matrix) {
  const Coefficients& c =
      resolveMatrix(matrix, height) == ColourMatrix::kBt709 ? kBt709 : kBt601;

  const int pairs = width / 2;

  for (int y = 0; y < height; ++y) {
    const uint8_t* srcRow = src + static_cast<size_t>(y) * srcStride;
    uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;

    for (int p = 0; p < pairs; ++p) {
      const uint8_t b0 = srcRow[0], g0 = srcRow[1], r0 = srcRow[2];
      const uint8_t b1 = srcRow[4], g1 = srcRow[5], r1 = srcRow[6];
      srcRow += 8;

      const int32_t y0 = round16(c.yr * r0 + c.yg * g0 + c.yb * b0 + c.yOffset);
      const int32_t y1 = round16(c.yr * r1 + c.yg * g1 + c.yb * b1 + c.yOffset);

      // Average the pair in RGB before the matrix, which is equivalent to
      // averaging the chroma and saves one matrix multiply per pixel.
      const int32_t rAvg = (static_cast<int32_t>(r0) + r1 + 1) >> 1;
      const int32_t gAvg = (static_cast<int32_t>(g0) + g1 + 1) >> 1;
      const int32_t bAvg = (static_cast<int32_t>(b0) + b1 + 1) >> 1;

      const int32_t cb =
          round16(c.ur * rAvg + c.ug * gAvg + c.ub * bAvg) + 128;
      const int32_t cr =
          round16(c.vr * rAvg + c.vg * gAvg + c.vb * bAvg) + 128;

      dstRow[0] = clampByte(cb);
      dstRow[1] = clampByte(y0);
      dstRow[2] = clampByte(cr);
      dstRow[3] = clampByte(y1);
      dstRow += 4;
    }
  }
}

void copyBgra(const uint8_t* src, int srcStride,
              uint8_t* dst, int dstStride,
              int width, int height,
              bool flipVertically) {
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const int srcY = flipVertically ? (height - 1 - y) : y;
    std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                src + static_cast<size_t>(srcY) * srcStride, rowBytes);
  }
}

void fillBlackBgra(uint8_t* dst, int stride, int width, int height) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      row[0] = 0;    // B
      row[1] = 0;    // G
      row[2] = 0;    // R
      row[3] = 255;  // A — opaque, so a key-less output does not go clear
      row += 4;
    }
  }
}

void fillBlackUyvy(uint8_t* dst, int stride, int width, int height) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; x += 2) {
      row[0] = 128;  // Cb
      row[1] = 16;   // Y0
      row[2] = 128;  // Cr
      row[3] = 16;   // Y1
      row += 4;
    }
  }
}

void downscaleBgra(const uint8_t* src, int srcStride, int srcWidth, int srcHeight,
                   uint8_t* dst, int dstStride, int factor) {
  if (factor < 1) {
    factor = 1;
  }
  const int dstWidth = srcWidth / factor;
  const int dstHeight = srcHeight / factor;
  const int samples = factor * factor;

  for (int y = 0; y < dstHeight; ++y) {
    uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;
    for (int x = 0; x < dstWidth; ++x) {
      int32_t b = 0, g = 0, r = 0;
      for (int sy = 0; sy < factor; ++sy) {
        const uint8_t* srcRow =
            src + static_cast<size_t>(y * factor + sy) * srcStride +
            static_cast<size_t>(x * factor) * 4;
        for (int sx = 0; sx < factor; ++sx) {
          b += srcRow[0];
          g += srcRow[1];
          r += srcRow[2];
          srcRow += 4;
        }
      }
      dstRow[0] = static_cast<uint8_t>(b / samples);
      dstRow[1] = static_cast<uint8_t>(g / samples);
      dstRow[2] = static_cast<uint8_t>(r / samples);
      dstRow[3] = 255;
      dstRow += 4;
    }
  }
}

}  // namespace weblinked
