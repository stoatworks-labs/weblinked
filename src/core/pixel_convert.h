#pragma once

#include <cstdint>

#include "core/video_format.h"

namespace weblinked {

/// Which matrix to use converting RGB to Y'CbCr. Getting this wrong does not
/// break anything visibly at first — it just shifts every colour slightly, and
/// is then blamed on the camera for the rest of the show.
enum class ColourMatrix {
  kBt601,
  kBt709,
  /// BT.601 below 720 lines, BT.709 at and above it. Matches what NDI, OMT and
  /// the DeckLink driver each assume when told nothing.
  kAuto,
};

ColourMatrix resolveMatrix(ColourMatrix matrix, int height);

/// BGRA (memory order B,G,R,A) to packed UYVY 4:2:2.
///
/// Output is studio-swing: Y in [16,235], Cb/Cr in [16,240], because that is
/// what SDI and both IP protocols carry. Chroma is averaged across each pixel
/// pair rather than point-sampled, which costs nothing here and avoids the
/// jagged edges you get on hard chroma transitions such as white text on
/// saturated backgrounds.
///
/// Alpha is discarded: UYVY has nowhere to put it.
///
/// `width` must be even. srcStride/dstStride are in bytes and may include
/// padding.
void bgraToUyvy(const uint8_t* src, int srcStride,
                uint8_t* dst, int dstStride,
                int width, int height,
                ColourMatrix matrix);

/// Straight copy, optionally flipping vertically. Needed because DeckLink and
/// the IP protocols all want the first row of memory to be the top row of the
/// picture, which is what CEF gives us, whereas an OpenGL-sourced frame would
/// not be.
void copyBgra(const uint8_t* src, int srcStride,
              uint8_t* dst, int dstStride,
              int width, int height,
              bool flipVertically);

/// Converts premultiplied BGRA to straight (unassociated) alpha, in place or
/// between buffers.
///
/// Chromium composites with premultiplied alpha — a 50%-opaque pure green
/// arrives as (0,128,0,128), not (0,255,0,128). NDI, OMT and a DeckLink keyer
/// all expect straight alpha, so passing Chromium's buffer through unchanged
/// makes every partially transparent pixel too dark: soft edges, drop shadows
/// and fades render muddy, and the error is invisible on fully opaque graphics,
/// which is why it survives casual testing.
///
/// Fully transparent pixels have no recoverable colour and are left at zero.
void unpremultiplyBgra(const uint8_t* src, int srcStride,
                       uint8_t* dst, int dstStride,
                       int width, int height);

/// Fills a BGRA buffer with opaque black. Used to pre-roll outputs before the
/// first paint arrives, so a card is never handed uninitialised memory.
void fillBlackBgra(uint8_t* dst, int stride, int width, int height);

/// Fills a UYVY buffer with legal black (Y=16, Cb=Cr=128).
void fillBlackUyvy(uint8_t* dst, int stride, int width, int height);

/// Box-downscales BGRA by an integer factor, for the control UI's preview.
/// Approximate on purpose: it is a confidence monitor, not a scope.
void downscaleBgra(const uint8_t* src, int srcStride, int srcWidth, int srcHeight,
                   uint8_t* dst, int dstStride, int factor);

}  // namespace weblinked
