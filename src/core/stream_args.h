#pragma once

#include <string>
#include <vector>

#include "core/video_format.h"

namespace weblinked {

/// Everything the stream output needs to build an ffmpeg command line.
///
/// This lives in `core`, away from the backend that uses it, for one reason:
/// the command line is the part that can be wrong in a way that only shows up
/// as a stream nobody can play, and `core` is the half of this project that has
/// cheap tests. See tests/test_stream_args.cpp.
struct StreamSettings {
  /// Where to publish, e.g. rtmp://host:1935/live/key or srt://host:6000
  std::string url;
  /// Container. Empty means "pick from the URL scheme": FLV for RTMP, MPEG-TS
  /// for SRT and UDP.
  std::string container;
  /// ffmpeg executable; looked up on PATH when it is a bare name.
  std::string ffmpegPath = "ffmpeg";

  std::string videoCodec = "libx264";
  std::string videoBitrate = "6000k";
  std::string preset = "veryfast";
  /// x264 tune; empty means don't pass one.
  std::string tune;
  /// Keyframe interval in seconds. Every platform that ingests RTMP wants 2.
  double gopSeconds = 2.0;

  std::string audioCodec = "aac";
  std::string audioBitrate = "128k";
  int audioChannels = 2;
  int audioSampleRate = 48000;

  /// Extra arguments spliced in before the output URL, for the case this
  /// struct has not thought of.
  std::vector<std::string> extraArgs;
};

/// The container ffmpeg should mux into for a URL, when none was configured.
std::string containerForUrl(const std::string& url);

/// A URL with its stream key replaced, safe to put in a log line, a status
/// response or a diagnostics bundle. A stream key is a password: anyone holding
/// one can broadcast to the channel.
std::string redactStreamUrl(const std::string& url);

/// Double a bitrate string ("6000k" -> "12000k") for the encoder's buffer.
/// Anything unparseable comes back unchanged.
std::string doubleBitrate(const std::string& bitrate);

/// The full ffmpeg argv, including argv[0].
///
/// Video and audio arrive as two raw streams over loopback TCP, neither
/// carrying timestamps: ffmpeg derives video pts from the frame count at `-r`
/// and audio pts from the sample count at `-ar`. The rate is therefore passed
/// as an exact rational — `60000/1001`, never `59.94` — because a decimal here
/// is a different rate and drifts a frame every few minutes.
std::vector<std::string> buildFfmpegArgs(const StreamSettings& settings,
                                         const VideoFormat& format,
                                         int videoPort,
                                         int audioPort);

}  // namespace weblinked
