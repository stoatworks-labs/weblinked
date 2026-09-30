#include "core/rotated_blit.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace weblinked {

bool blitRotationFromString(const std::string& text, BlitRotation& rotation) {
  if (text == "0") {
    rotation = BlitRotation::k0;
  } else if (text == "90") {
    rotation = BlitRotation::k90;
  } else if (text == "180") {
    rotation = BlitRotation::k180;
  } else if (text == "270" || text == "-90") {
    rotation = BlitRotation::k270;
  } else {
    return false;
  }
  return true;
}

const char* blitRotationToString(BlitRotation rotation) {
  switch (rotation) {
    case BlitRotation::k90:
      return "90";
    case BlitRotation::k180:
      return "180";
    case BlitRotation::k270:
      return "270";
    case BlitRotation::k0:
      break;
  }
  return "0";
}

BlitFit blitFitFromString(const std::string& text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "fill") return BlitFit::kFill;
  if (lower == "stretch") return BlitFit::kStretch;
  return BlitFit::kFit;
}

const char* blitFitToString(BlitFit fit) {
  switch (fit) {
    case BlitFit::kFill:
      return "fill";
    case BlitFit::kStretch:
      return "stretch";
    case BlitFit::kFit:
      break;
  }
  return "fit";
}

void RotatedBlit::configure(int sourceWidth, int sourceHeight, int destWidth,
                            int destHeight, BlitRotation rotation, BlitFit fit) {
  sourceWidth_ = std::max(sourceWidth, 0);
  sourceHeight_ = std::max(sourceHeight, 0);
  destWidth_ = std::max(destWidth, 0);
  destHeight_ = std::max(destHeight, 0);
  rotation_ = rotation;
  const bool transposed = rotation == BlitRotation::k90 || rotation == BlitRotation::k270;
  rotatedWidth_ = transposed ? sourceHeight_ : sourceWidth_;
  rotatedHeight_ = transposed ? sourceWidth_ : sourceHeight_;

  placement_ = BlitRect{};
  columnOf_.assign(static_cast<size_t>(destWidth_), 0);
  rowOf_.assign(static_cast<size_t>(destHeight_), 0);
  x0_ = x1_ = y0_ = y1_ = 0;
  identity_ = false;
  if (rotatedWidth_ == 0 || rotatedHeight_ == 0 || destWidth_ == 0 || destHeight_ == 0) {
    return;
  }

  // Integer arithmetic throughout, in 64 bits: the placement has to agree
  // exactly with the tables below, or a one-pixel seam of stale bars appears
  // between them on some rasters and not others.
  const int64_t rw = rotatedWidth_, rh = rotatedHeight_;
  const int64_t dw = destWidth_, dh = destHeight_;
  int64_t w = dw, h = dh;
  if (fit != BlitFit::kStretch) {
    // Compare rw/rh against dw/dh without dividing.
    const bool pictureIsWider = rw * dh > dw * rh;
    const bool fitWidth = (fit == BlitFit::kFit) == pictureIsWider;
    if (fitWidth) {
      w = dw;
      h = (rh * dw + rw / 2) / rw;
    } else {
      h = dh;
      w = (rw * dh + rh / 2) / rh;
    }
  }
  placement_.width = static_cast<int>(w);
  placement_.height = static_cast<int>(h);
  placement_.x = static_cast<int>((dw - w) / 2);
  placement_.y = static_cast<int>((dh - h) / 2);

  x0_ = std::max(placement_.x, 0);
  x1_ = std::min(placement_.x + placement_.width, destWidth_);
  y0_ = std::max(placement_.y, 0);
  y1_ = std::min(placement_.y + placement_.height, destHeight_);

  // Pixel centres: destination pixel i covers [i, i+1) and samples the rotated
  // picture at the centre of that span, which is what keeps a 2:1 downscale
  // from favouring one edge.
  for (int x = x0_; x < x1_; ++x) {
    const int64_t local = x - placement_.x;
    columnOf_[x] = static_cast<int>(std::min<int64_t>((2 * local + 1) * rw / (2 * w), rw - 1));
  }
  for (int y = y0_; y < y1_; ++y) {
    const int64_t local = y - placement_.y;
    rowOf_[y] = static_cast<int>(std::min<int64_t>((2 * local + 1) * rh / (2 * h), rh - 1));
  }
  identity_ = rotation == BlitRotation::k0 && w == rw && h == rh;
}

void RotatedBlit::clearBars(uint8_t* dest, int destRowBytes) const {
  for (int y = 0; y < destHeight_; ++y) {
    uint8_t* row = dest + static_cast<size_t>(y) * destRowBytes;
    if (y < y0_ || y >= y1_) {
      std::memset(row, 0, static_cast<size_t>(destWidth_) * 4);
      continue;
    }
    std::memset(row, 0, static_cast<size_t>(x0_) * 4);
    std::memset(row + static_cast<size_t>(x1_) * 4, 0,
                static_cast<size_t>(destWidth_ - x1_) * 4);
  }
}

void RotatedBlit::blit(const uint8_t* source, int sourceRowBytes, uint8_t* dest,
                       int destRowBytes) const {
  if (x1_ <= x0_ || y1_ <= y0_) {
    return;
  }

  if (identity_) {
    // The common signage case: a page rendered at the display's own raster.
    const size_t bytes = static_cast<size_t>(x1_ - x0_) * 4;
    for (int y = y0_; y < y1_; ++y) {
      std::memcpy(dest + static_cast<size_t>(y) * destRowBytes + static_cast<size_t>(x0_) * 4,
                  source + static_cast<size_t>(rowOf_[y]) * sourceRowBytes +
                      static_cast<size_t>(columnOf_[x0_]) * 4,
                  bytes);
    }
    return;
  }

  // Source address of destination pixel (x, y) is xOffset[x] + yOffset[y]; see
  // the class comment. With R the rotated picture and (c, r) = (columnOf[x],
  // rowOf[y]) a position in it, the source pixel is:
  //
  //     0:  (c,        r)            90: (r,        H - 1 - c)
  //   180:  (W - 1 - c, H - 1 - r)  270: (W - 1 - r, c)
  //
  // where W and H are the *source* width and height. Turning the picture 90°
  // clockwise puts its bottom-left corner at the top-left, which is the
  // (0, H - 1) the second case gives for c = r = 0.
  const size_t sw = static_cast<size_t>(sourceWidth_);
  const size_t sh = static_cast<size_t>(sourceHeight_);
  const size_t stride = static_cast<size_t>(sourceRowBytes);
  thread_local std::vector<size_t> xOffset;
  thread_local std::vector<size_t> yOffset;
  xOffset.resize(static_cast<size_t>(destWidth_));
  yOffset.resize(static_cast<size_t>(destHeight_));
  for (int x = x0_; x < x1_; ++x) {
    const size_t c = static_cast<size_t>(columnOf_[x]);
    switch (rotation_) {
      case BlitRotation::k0: xOffset[x] = c * 4; break;
      case BlitRotation::k90: xOffset[x] = (sh - 1 - c) * stride; break;
      case BlitRotation::k180: xOffset[x] = (sw - 1 - c) * 4; break;
      case BlitRotation::k270: xOffset[x] = c * stride; break;
    }
  }
  for (int y = y0_; y < y1_; ++y) {
    const size_t r = static_cast<size_t>(rowOf_[y]);
    switch (rotation_) {
      case BlitRotation::k0: yOffset[y] = r * stride; break;
      case BlitRotation::k90: yOffset[y] = r * 4; break;
      case BlitRotation::k180: yOffset[y] = (sh - 1 - r) * stride; break;
      case BlitRotation::k270: yOffset[y] = (sw - 1 - r) * 4; break;
    }
  }

  // Tiled so that a turned copy, which walks down source columns, reads each
  // source cache line once per tile rather than once per destination pixel.
  // 32 x 32 x 4 bytes is 4 KiB of destination and, turned, 32 source lines.
  constexpr int kTile = 32;
  for (int ty = y0_; ty < y1_; ty += kTile) {
    const int tyEnd = std::min(ty + kTile, y1_);
    for (int tx = x0_; tx < x1_; tx += kTile) {
      const int txEnd = std::min(tx + kTile, x1_);
      for (int y = ty; y < tyEnd; ++y) {
        const uint8_t* base = source + yOffset[y];
        auto* out = reinterpret_cast<uint32_t*>(dest + static_cast<size_t>(y) * destRowBytes);
        for (int x = tx; x < txEnd; ++x) {
          uint32_t pixel;
          std::memcpy(&pixel, base + xOffset[x], 4);
          out[x] = pixel;
        }
      }
    }
  }
}

}  // namespace weblinked
