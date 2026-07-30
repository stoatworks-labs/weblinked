#include "core/video_format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace weblinked {

int bytesPerPixelNumerator(PixelFormat format) {
  switch (format) {
    case PixelFormat::kBGRA:
      return 4;
    case PixelFormat::kUYVY:
      return 2;
  }
  return 4;
}

double FrameRate::toDouble() const {
  if (denominator == 0) {
    return 0.0;
  }
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

int64_t FrameRate::frameDurationTicks(int64_t timescale) const {
  if (numerator == 0) {
    return 0;
  }
  return (timescale * denominator) / numerator;
}

bool FrameRate::operator==(const FrameRate& other) const {
  // Compare as reduced fractions so 50/1 equals 100/2.
  const int a = std::gcd(numerator, denominator);
  const int b = std::gcd(other.numerator, other.denominator);
  if (a == 0 || b == 0) {
    return numerator == other.numerator && denominator == other.denominator;
  }
  return (numerator / a) == (other.numerator / b) &&
         (denominator / a) == (other.denominator / b);
}

std::optional<FrameRate> FrameRate::parse(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }

  const auto slash = text.find('/');
  if (slash != std::string::npos) {
    try {
      const int num = std::stoi(text.substr(0, slash));
      const int den = std::stoi(text.substr(slash + 1));
      if (num <= 0 || den <= 0) {
        return std::nullopt;
      }
      return FrameRate{num, den};
    } catch (...) {
      return std::nullopt;
    }
  }

  // Decimal forms. The three that matter are the 1000/1001 rates; anything
  // else decimal is treated as an exact integer rate or rejected.
  double value = 0.0;
  try {
    size_t consumed = 0;
    value = std::stod(text, &consumed);
    if (consumed != text.size()) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
  if (value <= 0.0) {
    return std::nullopt;
  }

  struct Fractional {
    double approx;
    int num;
    int den;
  };
  static const Fractional kFractional[] = {
      {23.976, 24000, 1001}, {23.98, 24000, 1001},  {29.97, 30000, 1001},
      {47.952, 48000, 1001}, {59.94, 60000, 1001},  {119.88, 120000, 1001},
  };
  for (const auto& f : kFractional) {
    if (std::fabs(value - f.approx) < 0.01) {
      return FrameRate{f.num, f.den};
    }
  }

  const double rounded = std::round(value);
  if (std::fabs(value - rounded) < 1e-9) {
    return FrameRate{static_cast<int>(rounded), 1};
  }
  return std::nullopt;
}

std::string FrameRate::toString() const {
  if (denominator == 1) {
    return std::to_string(numerator);
  }
  std::ostringstream out;
  // Two decimals is enough to name every rate in use, and reads better in a
  // UI than 60000/1001.
  out.setf(std::ios::fixed);
  out.precision(2);
  out << toDouble();
  std::string s = out.str();
  while (!s.empty() && s.back() == '0') {
    s.pop_back();
  }
  if (!s.empty() && s.back() == '.') {
    s.pop_back();
  }
  return s;
}

int VideoFormat::rowBytes(PixelFormat format) const {
  return width * bytesPerPixelNumerator(format);
}

size_t VideoFormat::bufferSize(PixelFormat format) const {
  return static_cast<size_t>(rowBytes(format)) * static_cast<size_t>(height);
}

double VideoFormat::aspectRatio() const {
  if (height == 0) {
    return 0.0;
  }
  return static_cast<double>(width) / static_cast<double>(height);
}

bool VideoFormat::operator==(const VideoFormat& other) const {
  return width == other.width && height == other.height &&
         rate == other.rate && interlaced == other.interlaced;
}

std::string VideoFormat::toString() const {
  std::ostringstream out;
  out << width << 'x' << height << (interlaced ? 'i' : 'p') << rate.toString();
  return out.str();
}

const std::vector<StandardRaster>& standardRasters() {
  static const std::vector<StandardRaster> kRasters = {
      {"480", 720, 486},    {"576", 720, 576},    {"720", 1280, 720},
      {"1080", 1920, 1080}, {"1440", 2560, 1440}, {"2160", 3840, 2160},
      {"4320", 7680, 4320},
      // DCI variants, which share a height with their UHD cousins and so
      // cannot use bare shorthand.
      {"2160dci", 4096, 2160},
      {"1080dci", 2048, 1080},
  };
  return kRasters;
}

std::optional<VideoFormat> VideoFormat::parse(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }

  std::string lowered = text;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // Find the scan-type separator: the last 'p' or 'i' that has digits after it.
  size_t split = std::string::npos;
  bool interlacedScan = false;
  for (size_t i = lowered.size(); i-- > 0;) {
    const char c = lowered[i];
    if ((c == 'p' || c == 'i') && i + 1 < lowered.size() &&
        std::isdigit(static_cast<unsigned char>(lowered[i + 1]))) {
      split = i;
      interlacedScan = (c == 'i');
      break;
    }
  }
  if (split == std::string::npos) {
    return std::nullopt;
  }

  const std::string rasterPart = lowered.substr(0, split);
  const std::string ratePart = lowered.substr(split + 1);

  const auto rate = FrameRate::parse(ratePart);
  if (!rate) {
    return std::nullopt;
  }

  VideoFormat format;
  format.rate = *rate;
  format.interlaced = interlacedScan;

  const auto x = rasterPart.find('x');
  if (x != std::string::npos) {
    try {
      format.width = std::stoi(rasterPart.substr(0, x));
      format.height = std::stoi(rasterPart.substr(x + 1));
    } catch (...) {
      return std::nullopt;
    }
  } else {
    bool matched = false;
    for (const auto& raster : standardRasters()) {
      if (rasterPart == raster.shorthand) {
        format.width = raster.width;
        format.height = raster.height;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return std::nullopt;
    }
  }

  if (format.width <= 0 || format.height <= 0) {
    return std::nullopt;
  }
  // 4:2:2 packing needs pairs of pixels; an odd width would silently truncate.
  if (format.width % 2 != 0) {
    return std::nullopt;
  }
  return format;
}

}  // namespace weblinked
