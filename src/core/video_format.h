#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weblinked {

/// Pixel layouts we can hand to an output. Names describe *memory* order, not
/// a vendor's FourCC convention, because those disagree with each other:
/// NDI's "BGRA" and OMT's "BGRA" and DeckLink's bmdFormat8BitBGRA all mean the
/// same bytes, while DeckLink's bmdFormat8BitARGB does not.
enum class PixelFormat {
  /// 32bpp, bytes in memory: B, G, R, A. What CEF paints.
  kBGRA,
  /// 16bpp 4:2:2, bytes in memory: U, Y0, V, Y1 for each pixel pair.
  kUYVY,
};

int bytesPerPixelNumerator(PixelFormat format);

/// A frame rate as an exact rational, never a double. 59.94 is 60000/1001 and
/// storing it as 59.94 accumulates a frame of drift every few minutes.
struct FrameRate {
  int numerator = 25;
  int denominator = 1;

  double toDouble() const;
  /// Duration of one frame in the given timescale, e.g. ticks(10'000'000) for
  /// OMT's 100ns units.
  int64_t frameDurationTicks(int64_t timescale) const;
  bool operator==(const FrameRate& other) const;

  /// Parses "50", "59.94", "60000/1001", "29.97". Rejects anything else rather
  /// than guessing.
  static std::optional<FrameRate> parse(const std::string& text);
  std::string toString() const;
};

/// A raster plus a rate plus scan type. This is the contract between the
/// browser source and every output.
struct VideoFormat {
  int width = 1920;
  int height = 1080;
  FrameRate rate{25, 1};
  bool interlaced = false;

  int rowBytes(PixelFormat format) const;
  size_t bufferSize(PixelFormat format) const;
  double aspectRatio() const;

  bool operator==(const VideoFormat& other) const;

  /// "1920x1080p50". Round-trips through parse().
  std::string toString() const;

  /// Accepts either the full form "1920x1080p50" or the broadcast shorthand
  /// "1080p50", "1080i25", "720p59.94", "2160p30". Shorthand heights resolve
  /// through the standard raster table below.
  static std::optional<VideoFormat> parse(const std::string& text);
};

/// The rasters an operator will actually ask for, so shorthand like "1080p50"
/// resolves without a lookup table in every backend.
struct StandardRaster {
  const char* shorthand;  // "1080"
  int width;
  int height;
};

const std::vector<StandardRaster>& standardRasters();

}  // namespace weblinked
