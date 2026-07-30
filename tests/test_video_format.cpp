#include "core/video_format.h"
#include "test_support.h"

using namespace weblinked;

WEBLINKED_TEST(frame_rate_parses_integers_and_fractions) {
  auto r50 = FrameRate::parse("50");
  CHECK(r50.has_value());
  CHECK_EQ(r50->numerator, 50);
  CHECK_EQ(r50->denominator, 1);

  auto explicitRatio = FrameRate::parse("60000/1001");
  CHECK(explicitRatio.has_value());
  CHECK_EQ(explicitRatio->numerator, 60000);
  CHECK_EQ(explicitRatio->denominator, 1001);

  // The decimal shorthand an operator will actually type must resolve to the
  // exact rational, not to 59.94 as a double.
  auto decimal = FrameRate::parse("59.94");
  CHECK(decimal.has_value());
  CHECK_EQ(decimal->numerator, 60000);
  CHECK_EQ(decimal->denominator, 1001);

  auto p2997 = FrameRate::parse("29.97");
  CHECK(p2997.has_value());
  CHECK_EQ(p2997->numerator, 30000);
  CHECK_EQ(p2997->denominator, 1001);

  auto p23976 = FrameRate::parse("23.976");
  CHECK(p23976.has_value());
  CHECK_EQ(p23976->numerator, 24000);
  CHECK_EQ(p23976->denominator, 1001);
}

WEBLINKED_TEST(frame_rate_rejects_nonsense) {
  CHECK(!FrameRate::parse("").has_value());
  CHECK(!FrameRate::parse("fifty").has_value());
  CHECK(!FrameRate::parse("0").has_value());
  CHECK(!FrameRate::parse("-25").has_value());
  CHECK(!FrameRate::parse("25fps").has_value());
  CHECK(!FrameRate::parse("50/0").has_value());
}

WEBLINKED_TEST(frame_rate_compares_as_reduced_fractions) {
  CHECK(FrameRate({50, 1}) == FrameRate({100, 2}));
  CHECK(!(FrameRate({50, 1}) == FrameRate({60000, 1001})));
}

WEBLINKED_TEST(frame_duration_ticks_are_exact) {
  // OMT and NDI both use 100 ns units.
  CHECK_EQ(FrameRate({25, 1}).frameDurationTicks(10'000'000), 400'000);
  CHECK_EQ(FrameRate({50, 1}).frameDurationTicks(10'000'000), 200'000);
  // 10^7 * 1001 / 60000 = 166833.33 -> truncates, and the remainder is why
  // presentation times are derived from a tick index rather than accumulated.
  CHECK_EQ(FrameRate({60000, 1001}).frameDurationTicks(10'000'000), 166'833);
}

WEBLINKED_TEST(video_format_parses_broadcast_shorthand) {
  auto hd = VideoFormat::parse("1080p50");
  CHECK(hd.has_value());
  CHECK_EQ(hd->width, 1920);
  CHECK_EQ(hd->height, 1080);
  CHECK_EQ(hd->rate.numerator, 50);
  CHECK(!hd->interlaced);

  auto interlaced = VideoFormat::parse("1080i25");
  CHECK(interlaced.has_value());
  CHECK(interlaced->interlaced);
  CHECK_EQ(interlaced->height, 1080);

  auto uhd = VideoFormat::parse("2160p30");
  CHECK(uhd.has_value());
  CHECK_EQ(uhd->width, 3840);
  CHECK_EQ(uhd->height, 2160);

  auto dci = VideoFormat::parse("2160dcip24");
  CHECK(dci.has_value());
  CHECK_EQ(dci->width, 4096);

  auto fractional = VideoFormat::parse("720p59.94");
  CHECK(fractional.has_value());
  CHECK_EQ(fractional->width, 1280);
  CHECK_EQ(fractional->rate.numerator, 60000);
  CHECK_EQ(fractional->rate.denominator, 1001);
}

WEBLINKED_TEST(video_format_parses_explicit_rasters) {
  auto explicitRaster = VideoFormat::parse("1920x1080p50");
  CHECK(explicitRaster.has_value());
  CHECK_EQ(explicitRaster->width, 1920);
  CHECK_EQ(explicitRaster->height, 1080);

  // A page can legitimately be an odd shape, e.g. an LED wall strip.
  auto wall = VideoFormat::parse("3840x600p60");
  CHECK(wall.has_value());
  CHECK_EQ(wall->height, 600);
}

WEBLINKED_TEST(video_format_rejects_what_cannot_be_carried) {
  CHECK(!VideoFormat::parse("1080").has_value());        // no rate
  CHECK(!VideoFormat::parse("p50").has_value());         // no raster
  CHECK(!VideoFormat::parse("999p50").has_value());      // unknown shorthand
  CHECK(!VideoFormat::parse("1920x1080p").has_value());  // no rate digits
  // Odd widths cannot be packed as 4:2:2 without silently losing a column.
  CHECK(!VideoFormat::parse("1921x1080p50").has_value());
}

WEBLINKED_TEST(video_format_round_trips_through_its_own_string) {
  for (const char* text : {"1920x1080p50", "1280x720p59.94", "3840x2160p30",
                           "1920x1080i25"}) {
    auto parsed = VideoFormat::parse(text);
    CHECK(parsed.has_value());
    if (parsed) {
      CHECK_STR(parsed->toString(), text);
      auto reparsed = VideoFormat::parse(parsed->toString());
      CHECK(reparsed.has_value());
      CHECK(reparsed.has_value() && *reparsed == *parsed);
    }
  }
}

WEBLINKED_TEST(buffer_sizes_match_the_pixel_format) {
  VideoFormat hd;
  hd.width = 1920;
  hd.height = 1080;

  CHECK_EQ(hd.rowBytes(PixelFormat::kBGRA), 1920 * 4);
  CHECK_EQ(hd.rowBytes(PixelFormat::kUYVY), 1920 * 2);
  CHECK_EQ(hd.bufferSize(PixelFormat::kBGRA), static_cast<size_t>(1920) * 1080 * 4);
  CHECK_EQ(hd.bufferSize(PixelFormat::kUYVY), static_cast<size_t>(1920) * 1080 * 2);
  CHECK_NEAR(hd.aspectRatio(), 16.0 / 9.0, 1e-9);
}
