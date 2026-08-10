#include "core/stream_args.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace weblinked {
namespace {

bool startsWith(const std::string& text, const char* prefix) {
  const std::string p(prefix);
  return text.size() >= p.size() && text.compare(0, p.size(), p) == 0;
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

}  // namespace

std::string containerForUrl(const std::string& url) {
  const std::string lowered = lowercase(url);
  if (startsWith(lowered, "rtmp://") || startsWith(lowered, "rtmps://") ||
      startsWith(lowered, "rtmpt://")) {
    return "flv";
  }
  if (startsWith(lowered, "srt://") || startsWith(lowered, "udp://") ||
      startsWith(lowered, "rtp://")) {
    return "mpegts";
  }
  // An http(s) target is almost always an HLS or DASH ingest; anything else we
  // have no basis to guess about, and FLV is what RTMP-shaped endpoints want.
  return "flv";
}

std::string redactStreamUrl(const std::string& url) {
  // An RTMP stream key is the last path segment: rtmp://host/app/<key>. Keeping
  // the app path makes the log line still useful for telling two channels apart.
  const auto scheme = url.find("://");
  const auto pathStart = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  if (pathStart == std::string::npos) {
    return url;
  }
  const auto lastSlash = url.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash <= pathStart) {
    return url;
  }
  const std::string key = url.substr(lastSlash + 1);
  if (key.empty()) {
    return url;
  }
  return url.substr(0, lastSlash + 1) + "<key>";
}

std::string doubleBitrate(const std::string& bitrate) {
  if (bitrate.empty()) {
    return bitrate;
  }
  const char suffix = bitrate.back();
  const bool hasSuffix = (suffix == 'k' || suffix == 'K' || suffix == 'm' || suffix == 'M');
  const std::string digits = hasSuffix ? bitrate.substr(0, bitrate.size() - 1) : bitrate;
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return bitrate;
  }
  const long value = std::strtol(digits.c_str(), nullptr, 10);
  if (value <= 0) {
    return bitrate;
  }
  return std::to_string(value * 2) + (hasSuffix ? std::string(1, suffix) : std::string());
}

std::vector<std::string> buildFfmpegArgs(const StreamSettings& settings,
                                         const VideoFormat& format,
                                         int videoPort,
                                         int audioPort) {
  const std::string rate = std::to_string(format.rate.numerator) + "/" +
                           std::to_string(format.rate.denominator);
  const std::string size =
      std::to_string(format.width) + "x" + std::to_string(format.height);
  const std::string container =
      settings.container.empty() ? containerForUrl(settings.url) : settings.container;

  std::vector<std::string> args{
      settings.ffmpegPath,
      // -nostdin because ffmpeg otherwise puts the terminal into a mode that
      // outlives it, and this process has no terminal to give it anyway.
      "-hide_banner", "-nostdin", "-loglevel", "warning",

      // video in
      "-f", "rawvideo",
      "-pix_fmt", "uyvy422",
      "-s", size,
      "-r", rate,
      "-i", "tcp://127.0.0.1:" + std::to_string(videoPort),

      // audio in — f32le because that is the layout the engine already has
      // interleaved for the SDI backends, so nothing has to convert.
      "-f", "f32le",
      "-ar", std::to_string(settings.audioSampleRate),
      "-ac", std::to_string(settings.audioChannels),
      "-i", "tcp://127.0.0.1:" + std::to_string(audioPort),

      // Be explicit rather than trusting ffmpeg's stream selection: with one
      // input of each kind it would pick correctly today and silently differently
      // if a third input were ever added.
      "-map", "0:v:0",
      "-map", "1:a:0",
  };

  args.push_back("-c:v");
  args.push_back(settings.videoCodec);
  if (settings.videoCodec != "copy") {
    args.push_back("-preset");
    args.push_back(settings.preset);
    if (!settings.tune.empty()) {
      args.push_back("-tune");
      args.push_back(settings.tune);
    }
    // 4:2:0 8-bit: what every RTMP ingest accepts, and what x264 would warn
    // about being asked to change if UYVY were passed through as 4:2:2.
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");
    args.push_back("-b:v");
    args.push_back(settings.videoBitrate);
    args.push_back("-maxrate");
    args.push_back(settings.videoBitrate);
    args.push_back("-bufsize");
    args.push_back(doubleBitrate(settings.videoBitrate));

    const double fps = format.rate.toDouble();
    const long gop = std::lround(fps * (settings.gopSeconds > 0 ? settings.gopSeconds : 2.0));
    args.push_back("-g");
    args.push_back(std::to_string(gop > 0 ? gop : 1));
    // Without this x264 inserts keyframes on scene changes too, and a keyframe
    // interval a player cannot predict makes HLS segmenting downstream ragged.
    args.push_back("-sc_threshold");
    args.push_back("0");
  }

  args.push_back("-c:a");
  args.push_back(settings.audioCodec);
  if (settings.audioCodec != "copy") {
    args.push_back("-b:a");
    args.push_back(settings.audioBitrate);
  }

  for (const auto& extra : settings.extraArgs) {
    args.push_back(extra);
  }

  args.push_back("-f");
  args.push_back(container);
  args.push_back(settings.url);
  return args;
}

}  // namespace weblinked
