// Tests for the ffmpeg command line the stream output builds.
//
// This is the one part of an RTMP sender that can be wrong in a way nothing
// catches: a stream with a decimal frame rate, a swapped `-map`, or a missing
// `-pix_fmt` still connects, still shows a bitrate climbing, and still produces
// something a player will open. The fault only appears as drift over twenty
// minutes, or as a feed one platform accepts and another rejects.
//
// The backend itself needs a real ffmpeg and a real server; these run in
// milliseconds and pin the parts that don't.

#include <algorithm>
#include <string>
#include <vector>

#include "core/stream_args.h"
#include "test_support.h"

using namespace weblinked;

namespace {

VideoFormat format1080p50() {
  VideoFormat format;
  format.width = 1920;
  format.height = 1080;
  format.rate = FrameRate{50, 1};
  format.interlaced = false;
  return format;
}

VideoFormat format1080p5994() {
  VideoFormat format = format1080p50();
  format.rate = FrameRate{60000, 1001};
  return format;
}

/// The value following `flag`, or "" if the flag is absent.
std::string valueAfter(const std::vector<std::string>& args, const std::string& flag) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) {
      return args[i + 1];
    }
  }
  return {};
}

bool contains(const std::vector<std::string>& args, const std::string& value) {
  return std::find(args.begin(), args.end(), value) != args.end();
}

size_t indexOf(const std::vector<std::string>& args, const std::string& value) {
  const auto it = std::find(args.begin(), args.end(), value);
  return it == args.end() ? args.size() : static_cast<size_t>(it - args.begin());
}

StreamSettings rtmpSettings() {
  StreamSettings settings;
  settings.url = "rtmp://restreamer.local:1935/live/cam-a";
  return settings;
}

}  // namespace

WEBLINKED_TEST(stream_args_carry_an_exact_rational_rate) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p5994(), 5000, 5001);
  // 59.94 is 60000/1001. A decimal here loses a frame every few minutes, and
  // because ffmpeg accepts it the stream looks perfectly healthy while it does.
  CHECK_STR(valueAfter(args, "-r"), "60000/1001");
  CHECK(!contains(args, "59.94"));
}

WEBLINKED_TEST(stream_args_describe_the_raw_inputs_completely) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p50(), 5000, 5001);
  // rawvideo carries no header at all: every one of these has to be told to
  // ffmpeg or it guesses, and a wrong guess is a green sheared picture.
  CHECK_STR(valueAfter(args, "-pix_fmt"), "uyvy422");
  CHECK_STR(valueAfter(args, "-s"), "1920x1080");
  CHECK(contains(args, "tcp://127.0.0.1:5000"));
  CHECK(contains(args, "tcp://127.0.0.1:5001"));
  CHECK_STR(valueAfter(args, "-ar"), "48000");
  CHECK_STR(valueAfter(args, "-ac"), "2");
  CHECK_STR(valueAfter(args, "-f"), "rawvideo");
}

WEBLINKED_TEST(stream_args_map_video_from_input_0_and_audio_from_input_1) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p50(), 5000, 5001);
  CHECK(contains(args, "0:v:0"));
  CHECK(contains(args, "1:a:0"));
  // The video input must come first, or the maps point at the wrong streams.
  CHECK(indexOf(args, "tcp://127.0.0.1:5000") < indexOf(args, "tcp://127.0.0.1:5001"));
}

WEBLINKED_TEST(stream_args_end_with_the_container_and_the_url) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p50(), 5000, 5001);
  CHECK_EQ(args.size() >= 3, true);
  CHECK_STR(args.back(), "rtmp://restreamer.local:1935/live/cam-a");
  CHECK_STR(args[args.size() - 2], "flv");
  CHECK_STR(args[args.size() - 3], "-f");
}

WEBLINKED_TEST(stream_args_pick_the_container_from_the_url_scheme) {
  CHECK_STR(containerForUrl("rtmp://host/live/key"), "flv");
  CHECK_STR(containerForUrl("rtmps://host/live/key"), "flv");
  CHECK_STR(containerForUrl("RTMP://HOST/live/key"), "flv");
  CHECK_STR(containerForUrl("srt://host:6000"), "mpegts");
  CHECK_STR(containerForUrl("udp://239.0.0.1:1234"), "mpegts");

  StreamSettings settings = rtmpSettings();
  settings.url = "srt://host:6000";
  const auto args = buildFfmpegArgs(settings, format1080p50(), 5000, 5001);
  CHECK_STR(args[args.size() - 2], "mpegts");
}

WEBLINKED_TEST(stream_args_set_a_keyframe_every_two_seconds) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p50(), 5000, 5001);
  CHECK_STR(valueAfter(args, "-g"), "100");  // 50 fps x 2 s

  const auto args5994 = buildFfmpegArgs(rtmpSettings(), format1080p5994(), 5000, 5001);
  CHECK_STR(valueAfter(args5994, "-g"), "120");  // 59.94 x 2, rounded
}

WEBLINKED_TEST(stream_args_encode_to_what_an_rtmp_ingest_accepts) {
  const auto args = buildFfmpegArgs(rtmpSettings(), format1080p50(), 5000, 5001);
  CHECK_STR(valueAfter(args, "-c:v"), "libx264");
  CHECK_STR(valueAfter(args, "-c:a"), "aac");
  // 4:2:2 in, 4:2:0 out: no RTMP ingest takes 4:2:2 H.264.
  CHECK(contains(args, "yuv420p"));
  CHECK_STR(valueAfter(args, "-bufsize"), "12000k");  // twice the bitrate
}

WEBLINKED_TEST(stream_args_omit_encoder_settings_when_copying) {
  StreamSettings settings = rtmpSettings();
  settings.videoCodec = "copy";
  settings.audioCodec = "copy";
  const auto args = buildFfmpegArgs(settings, format1080p50(), 5000, 5001);
  CHECK_STR(valueAfter(args, "-c:v"), "copy");
  // -preset on a copy is an error, not a no-op.
  CHECK(!contains(args, "-preset"));
  CHECK(!contains(args, "-b:v"));
  CHECK(!contains(args, "-b:a"));
}

WEBLINKED_TEST(stream_url_redaction_hides_the_key_and_keeps_the_path) {
  // A stream key is a password: whoever holds it can broadcast to the channel.
  // It must not reach a log line, a status response or a diagnostics bundle.
  CHECK_STR(redactStreamUrl("rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl"),
            "rtmp://a.rtmp.youtube.com/live2/<key>");
  CHECK_STR(redactStreamUrl("rtmp://restreamer.local:1935/live/cam-a"),
            "rtmp://restreamer.local:1935/live/<key>");
  // Nothing to redact, nothing changed.
  CHECK_STR(redactStreamUrl("srt://host:6000"), "srt://host:6000");
}

WEBLINKED_TEST(stream_bitrate_doubling_survives_the_suffixes) {
  CHECK_STR(doubleBitrate("6000k"), "12000k");
  CHECK_STR(doubleBitrate("3M"), "6M");
  CHECK_STR(doubleBitrate("4500000"), "9000000");
  CHECK_STR(doubleBitrate("not-a-bitrate"), "not-a-bitrate");
  CHECK_STR(doubleBitrate(""), "");
}
