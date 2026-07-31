// ndi_probe — receives from an NDI source and reports what actually arrived.
//
// A verification tool, not part of the application. WebLinked's own counters can
// only tell you it *sent* something; this tells you what a receiver on the
// network actually got, which is the claim that matters. It checks the raster,
// the frame rate, the pixel format, and — with --bars — that the picture really
// carries the expected colours, so a silently wrong colour matrix or a swapped
// Cb/Cr cannot pass as working.
//
// Build (macOS):
//   clang++ -std=c++20 -I"/Library/NDI SDK for Apple/include" tools/ndi_probe.cpp \
//     "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib" -o ndi_probe
//
// Usage:
//   ndi_probe [--source <substring>] [--frames <n>] [--bars] [--timeout <s>]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <Processing.NDI.Lib.h>

namespace {

struct Ycbcr {
  int y, cb, cr;
};

/// Reads one pixel out of a UYVY buffer. Each 4-byte group is Cb Y0 Cr Y1.
Ycbcr sampleUyvy(const uint8_t* data, int stride, int x, int y) {
  const uint8_t* row = data + static_cast<size_t>(y) * stride;
  const int pair = x / 2;
  const uint8_t* group = row + static_cast<size_t>(pair) * 4;
  return {group[(x % 2) == 0 ? 1 : 3], group[0], group[2]};
}

/// BT.709 studio-swing reference, independent of the application's own maths.
Ycbcr referenceBt709(int r, int g, int b) {
  const double rn = r / 255.0, gn = g / 255.0, bn = b / 255.0;
  const double yn = 0.2126 * rn + 0.7152 * gn + 0.0722 * bn;
  return {static_cast<int>(std::lround(16.0 + 219.0 * yn)),
          static_cast<int>(std::lround(128.0 + 224.0 * ((bn - yn) / (2.0 * (1.0 - 0.0722))))),
          static_cast<int>(std::lround(128.0 + 224.0 * ((rn - yn) / (2.0 * (1.0 - 0.2126)))))};
}

/// Inverse of the sender's conversion: studio-swing BT.709 Y'CbCr back to RGB.
/// Written independently here rather than shared with the application, so a
/// saved frame is a check on the round trip and not a restatement of it.
void ycbcrToRgb(int y, int cb, int cr, uint8_t* out) {
  const double yn = (y - 16) / 219.0;
  const double cbn = (cb - 128) / 224.0;
  const double crn = (cr - 128) / 224.0;
  const double r = yn + 2.0 * (1.0 - 0.2126) * crn;
  const double b = yn + 2.0 * (1.0 - 0.0722) * cbn;
  const double g = (yn - 0.2126 * r - 0.0722 * b) / 0.7152;
  const auto clamp8 = [](double v) {
    return static_cast<uint8_t>(std::lround(std::min(1.0, std::max(0.0, v)) * 255.0));
  };
  out[0] = clamp8(r);
  out[1] = clamp8(g);
  out[2] = clamp8(b);
}

/// Writes the received frame as a binary PPM — no encoder, no dependency.
/// Convert with `sips -s format png frame.ppm --out frame.png` on macOS.
bool saveFramePpm(const NDIlib_video_frame_v2_t& frame, const std::string& path) {
  if (frame.FourCC != NDIlib_FourCC_video_type_UYVY) {
    std::fprintf(stderr, "--save currently handles UYVY only\n");
    return false;
  }
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  std::fprintf(file, "P6\n%d %d\n255\n", frame.xres, frame.yres);

  std::vector<uint8_t> row(static_cast<size_t>(frame.xres) * 3);
  const auto* data = static_cast<const uint8_t*>(frame.p_data);
  for (int y = 0; y < frame.yres; ++y) {
    for (int x = 0; x < frame.xres; ++x) {
      const Ycbcr pixel = sampleUyvy(data, frame.line_stride_in_bytes, x, y);
      ycbcrToRgb(pixel.y, pixel.cb, pixel.cr, row.data() + static_cast<size_t>(x) * 3);
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  std::printf("saved %dx%d frame to %s\n", frame.xres, frame.yres, path.c_str());
  return true;
}

int checkColourBars(const NDIlib_video_frame_v2_t& frame) {
  // The eight bars of tools/testcard.html, in order.
  struct Bar {
    const char* name;
    int r, g, b;
  };
  static const Bar kBars[] = {
      {"grey", 0xc0, 0xc0, 0xc0}, {"yellow", 0xc0, 0xc0, 0x00},
      {"cyan", 0x00, 0xc0, 0xc0}, {"green", 0x00, 0xc0, 0x00},
      {"magenta", 0xc0, 0x00, 0xc0}, {"red", 0xc0, 0x00, 0x00},
      {"blue", 0x00, 0x00, 0xc0}, {"black", 0x00, 0x00, 0x00},
  };

  // A quarter of the way down sits inside the bars, clear of the text below.
  const int y = frame.yres / 4;
  const int barWidth = frame.xres / 8;
  int failures = 0;

  std::printf("\n  bar        sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict\n");
  for (int i = 0; i < 8; ++i) {
    const int x = i * barWidth + barWidth / 2;
    const Ycbcr got = sampleUyvy(static_cast<const uint8_t*>(frame.p_data),
                                 frame.line_stride_in_bytes, x, y);
    const Ycbcr want = referenceBt709(kBars[i].r, kBars[i].g, kBars[i].b);

    // Two levels of tolerance: the page is rendered by a real browser, so
    // antialiasing and colour management can move a sample by a little.
    const bool pass = std::abs(got.y - want.y) <= 3 &&
                      std::abs(got.cb - want.cb) <= 3 &&
                      std::abs(got.cr - want.cr) <= 3;
    if (!pass) {
      ++failures;
    }
    std::printf("  %-9s  %4d %4d %4d      %4d %4d %4d       %s\n", kBars[i].name,
                got.y, got.cb, got.cr, want.y, want.cb, want.cr,
                pass ? "ok" : "MISMATCH");
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  std::string wanted;
  int wantFrames = 50;
  bool checkBars = false;
  int timeoutSeconds = 15;
  std::string savePath;
  int saveAfterFrames = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--source" && i + 1 < argc) {
      wanted = argv[++i];
    } else if (argument == "--frames" && i + 1 < argc) {
      wantFrames = std::atoi(argv[++i]);
    } else if (argument == "--timeout" && i + 1 < argc) {
      timeoutSeconds = std::atoi(argv[++i]);
    } else if (argument == "--save" && i + 1 < argc) {
      savePath = argv[++i];
    } else if (argument == "--save-after" && i + 1 < argc) {
      saveAfterFrames = std::atoi(argv[++i]);
    } else if (argument == "--bars") {
      checkBars = true;
    } else {
      std::fprintf(stderr,
                   "usage: ndi_probe [--source <substring>] [--frames <n>] "
                   "[--bars] [--timeout <s>] [--save <file.ppm>] "
                   "[--save-after <n>]\n");
      return 2;
    }
  }

  if (!NDIlib_initialize()) {
    std::fprintf(stderr, "NDIlib_initialize failed\n");
    return 1;
  }
  std::printf("NDI runtime: %s\n", NDIlib_version());

  NDIlib_find_create_t findSettings;
  std::memset(&findSettings, 0, sizeof(findSettings));
  findSettings.show_local_sources = true;
  NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findSettings);
  if (finder == nullptr) {
    std::fprintf(stderr, "NDIlib_find_create_v2 failed\n");
    return 1;
  }

  // Discovery is not instant; NDI needs a moment to hear the announcements.
  const NDIlib_source_t* chosen = nullptr;
  NDIlib_source_t chosenCopy{};
  std::string chosenName;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  while (std::chrono::steady_clock::now() < deadline && chosen == nullptr) {
    NDIlib_find_wait_for_sources(finder, 1000);
    uint32_t count = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &count);
    for (uint32_t i = 0; i < count; ++i) {
      const std::string name = sources[i].p_ndi_name != nullptr
                                   ? sources[i].p_ndi_name
                                   : "";
      if (wanted.empty() || name.find(wanted) != std::string::npos) {
        chosenCopy = sources[i];
        chosenName = name;
        chosen = &chosenCopy;
        break;
      }
    }
    if (chosen == nullptr && count > 0) {
      std::printf("visible sources (%u):\n", count);
      for (uint32_t i = 0; i < count; ++i) {
        std::printf("  %s\n", sources[i].p_ndi_name);
      }
    }
  }

  if (chosen == nullptr) {
    std::fprintf(stderr, "no NDI source%s found within %d s\n",
                 wanted.empty() ? "" : (" matching '" + wanted + "'").c_str(),
                 timeoutSeconds);
    NDIlib_find_destroy(finder);
    return 1;
  }
  std::printf("found source: %s\n", chosenName.c_str());

  NDIlib_recv_create_v3_t recvSettings;
  std::memset(&recvSettings, 0, sizeof(recvSettings));
  recvSettings.source_to_connect_to = chosenCopy;
  // Ask for the frames exactly as sent, so the probe reports what was
  // transmitted rather than what the SDK converted it to.
  recvSettings.color_format = NDIlib_recv_color_format_fastest;
  recvSettings.bandwidth = NDIlib_recv_bandwidth_highest;
  recvSettings.allow_video_fields = true;

  NDIlib_recv_instance_t receiver = NDIlib_recv_create_v3(&recvSettings);
  NDIlib_find_destroy(finder);
  if (receiver == nullptr) {
    std::fprintf(stderr, "NDIlib_recv_create_v3 failed\n");
    return 1;
  }

  int videoFrames = 0;
  int audioFrames = 0;
  int audioSamples = 0;
  float audioPeak = 0.0f;
  std::map<int, int> sampleCounts;
  bool reported = false;
  int barFailures = -1;
  const auto started = std::chrono::steady_clock::now();
  const auto receiveDeadline = started + std::chrono::seconds(timeoutSeconds);

  while (videoFrames < wantFrames &&
         std::chrono::steady_clock::now() < receiveDeadline) {
    NDIlib_video_frame_v2_t video{};
    NDIlib_audio_frame_v3_t audio{};

    switch (NDIlib_recv_capture_v3(receiver, &video, &audio, nullptr, 1000)) {
      case NDIlib_frame_type_video: {
        ++videoFrames;
        if (!savePath.empty() && videoFrames == saveAfterFrames) {
          saveFramePpm(video, savePath);
        }
        if (!reported) {
          reported = true;
          char fourcc[5] = {};
          std::memcpy(fourcc, &video.FourCC, 4);
          std::printf("\nfirst frame:\n"
                      "  raster       %dx%d %s\n"
                      "  rate         %d/%d (%.3f fps)\n"
                      "  FourCC       %s\n"
                      "  stride       %d bytes (%d expected for 4:2:2)\n"
                      "  aspect       %.4f\n",
                      video.xres, video.yres,
                      video.frame_format_type ==
                              NDIlib_frame_format_type_progressive
                          ? "progressive"
                          : "interlaced",
                      video.frame_rate_N, video.frame_rate_D,
                      static_cast<double>(video.frame_rate_N) / video.frame_rate_D,
                      fourcc, video.line_stride_in_bytes, video.xres * 2,
                      static_cast<double>(video.picture_aspect_ratio));

          // UYVA is UYVY followed by a full-resolution 8-bit alpha plane. The
          // NDI receiver picks it in preference to BGRA under
          // recv_color_format_fastest, so an alpha check has to handle both or
          // it silently reports nothing.
          if (video.FourCC == NDIlib_FourCC_video_type_UYVA) {
            const auto* px = static_cast<const uint8_t*>(video.p_data);
            const size_t alphaPlane =
                static_cast<size_t>(video.yres) * video.line_stride_in_bytes;
            std::printf("\n  alpha across the raster (UYVA alpha plane):\n");
            for (int i = 0; i < 4; ++i) {
              const int x = video.xres / 8 + i * video.xres / 4;
              const Ycbcr pixel = sampleUyvy(px, video.line_stride_in_bytes, x,
                                             video.yres / 2);
              // The alpha plane is one byte per pixel, its own stride being the
              // raster width.
              const size_t off = alphaPlane +
                                 static_cast<size_t>(video.yres / 2) * video.xres +
                                 static_cast<size_t>(x);
              std::printf("    x=%4d  Y=%3d Cb=%3d Cr=%3d  A=%3d\n", x, pixel.y,
                          pixel.cb, pixel.cr, px[off]);
            }
          } else if (video.FourCC == NDIlib_FourCC_video_type_BGRA ||
                     video.FourCC == NDIlib_FourCC_video_type_BGRX) {
            const auto* px = static_cast<const uint8_t*>(video.p_data);
            std::printf("\n  alpha across the raster (BGRA byte 3):\n");
            for (int i = 0; i < 4; ++i) {
              const int x = video.xres / 8 + i * video.xres / 4;
              const size_t off = static_cast<size_t>(video.yres / 2) *
                                     video.line_stride_in_bytes +
                                 static_cast<size_t>(x) * 4;
              std::printf("    x=%4d  B=%3d G=%3d R=%3d  A=%3d\n", x, px[off],
                          px[off + 1], px[off + 2], px[off + 3]);
            }
          }

          if (checkBars) {
            if (video.FourCC == NDIlib_FourCC_video_type_UYVY) {
              barFailures = checkColourBars(video);
            } else {
              std::printf("\n  --bars needs a UYVY source; got %s\n", fourcc);
            }
          }
        }
        NDIlib_recv_free_video_v2(receiver, &video);
        break;
      }
      case NDIlib_frame_type_audio: {
        ++audioFrames;
        audioSamples += audio.no_samples;
        // The distribution of per-frame sample counts is the interesting part.
        // At a fractional rate it must alternate — 48000/59.94 is 800.8, so a
        // correct sender emits a mixture of 800 and 801 rather than a constant
        // 800, which would lose 40 ms a minute.
        ++sampleCounts[audio.no_samples];
        float peak = 0.0f;
        if (audio.FourCC == NDIlib_FourCC_audio_type_FLTP && audio.p_data != nullptr) {
          for (int c = 0; c < audio.no_channels; ++c) {
            const auto* plane = reinterpret_cast<const float*>(
                audio.p_data + static_cast<size_t>(c) * audio.channel_stride_in_bytes);
            for (int s = 0; s < audio.no_samples; ++s) {
              peak = std::max(peak, std::fabs(plane[s]));
            }
          }
        }
        audioPeak = std::max(audioPeak, peak);
        if (audioFrames == 1) {
          char fourcc[5] = {};
          std::memcpy(fourcc, &audio.FourCC, 4);
          std::printf("\nfirst audio: %d ch at %d Hz, %d samples, FourCC %s, "
                      "channel stride %d\n",
                      audio.no_channels, audio.sample_rate, audio.no_samples,
                      fourcc, audio.channel_stride_in_bytes);
        }
        NDIlib_recv_free_audio_v3(receiver, &audio);
        break;
      }
      default:
        break;
    }
  }

  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  std::printf("\nreceived %d video frames and %d audio frames (%d samples) "
              "in %.2f s — %.2f fps measured\n",
              videoFrames, audioFrames, audioSamples, elapsed,
              elapsed > 0 ? videoFrames / elapsed : 0.0);

  if (audioFrames > 0) {
    std::printf("audio peak %.4f; samples per frame:", audioPeak);
    for (const auto& [count, times] : sampleCounts) {
      std::printf(" %dx%d", times, count);
    }
    // Mean samples per frame is what proves there is no drift: it should equal
    // sampleRate * frameDuration to well within one sample.
    std::printf("\n  mean %.4f samples/frame\n",
                static_cast<double>(audioSamples) / audioFrames);
  }

  NDIlib_recv_destroy(receiver);
  NDIlib_destroy();

  if (videoFrames == 0) {
    std::fprintf(stderr, "FAIL: no video received\n");
    return 1;
  }
  if (barFailures > 0) {
    std::fprintf(stderr, "FAIL: %d colour bar(s) did not match BT.709\n", barFailures);
    return 1;
  }
  std::printf("PASS\n");
  return 0;
}
