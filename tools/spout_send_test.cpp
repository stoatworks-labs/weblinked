// spout_send_test — drives the real Spout backend without the rest of WebLinked.
//
// Why this exists at all: the NDI and Syphon paths are verified by running the
// application itself and pointing an independent receiver at it. That is not
// available on Windows, because WebLinked has never been built there — CEF and
// everything above it is untested on this platform. So rather than claim the
// Spout output works because it compiles, this links the *actual*
// `src/outputs/shared_surface_win.cpp` against the *actual* vendored Spout SDK,
// hands it a `VideoFrame` exactly as the engine would, and lets `tools/spout_probe`
// check what comes out the other side.
//
// What it therefore does and does not prove:
//   * proves — SharedSurface::open/publish/close drive spoutDX correctly, the
//     BGRA frame reaches a receiver intact, the pitch is honoured, orientation
//     is preserved, and the name an operator types is the name that appears.
//   * does NOT prove — anything above the output: no CEF, no clock, no engine,
//     no control API. Those remain unbuilt on Windows.
//
// The pattern is the same four bands as tools/alphabars.html, at the same
// premultiplied values, so the numbers are directly comparable with the Syphon
// verification in docs/04-verification.md section 23.
//
// Build: tools/build_spout_test.ps1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "core/frame.h"
#include "core/video_format.h"
#include "outputs/shared_surface.h"

namespace {

/// tools/alphabars.html as premultiplied BGRA: opaque red, 50% green, 25%
/// blue, nothing. Chromium would premultiply these itself; here they are
/// written directly, which is what the real frame would contain by the time it
/// reached an output.
struct Band {
  uint8_t b, g, r, a;
};
constexpr Band kBands[4] = {
    {0, 0, 192, 255},
    {0, 96, 0, 128},
    {48, 0, 0, 64},
    {0, 0, 0, 0},
};

/// Top half red, bottom half blue — tools/updown.html, for the orientation
/// check that vertical bands cannot make.
void fillUpDown(weblinked::VideoFrame& frame) {
  const int width = frame.format().width;
  const int height = frame.format().height;
  for (int y = 0; y < height; ++y) {
    auto* row = frame.data() + (static_cast<size_t>(y) * frame.rowBytes());
    const Band band = (y < height / 2) ? Band{0, 0, 192, 255} : Band{192, 0, 0, 255};
    for (int x = 0; x < width; ++x) {
      row[x * 4 + 0] = band.b;
      row[x * 4 + 1] = band.g;
      row[x * 4 + 2] = band.r;
      row[x * 4 + 3] = band.a;
    }
  }
}

void fillAlphaBars(weblinked::VideoFrame& frame) {
  const int width = frame.format().width;
  const int height = frame.format().height;
  for (int y = 0; y < height; ++y) {
    auto* row = frame.data() + (static_cast<size_t>(y) * frame.rowBytes());
    for (int x = 0; x < width; ++x) {
      const Band band = kBands[(x * 4) / width];
      row[x * 4 + 0] = band.b;
      row[x * 4 + 1] = band.g;
      row[x * 4 + 2] = band.r;
      row[x * 4 + 3] = band.a;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string name = "WLTest";
  std::string pattern = "alphabars";
  int seconds = 30;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--name" && i + 1 < argc) {
      name = argv[++i];
    } else if (arg == "--pattern" && i + 1 < argc) {
      pattern = argv[++i];
    } else if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: spout_send_test [--name <sender>] "
                   "[--pattern alphabars|updown] [--seconds <n>]\n");
      return 2;
    }
  }

  weblinked::VideoFormat format;
  format.width = 1920;
  format.height = 1080;
  format.rate = weblinked::FrameRate{50, 1};

  weblinked::VideoFrame frame(format, weblinked::PixelFormat::kBGRA);
  if (pattern == "updown") {
    fillUpDown(frame);
  } else {
    fillAlphaBars(frame);
  }

  auto surface = weblinked::createSharedSurface();
  std::string error;
  if (!surface->open(format, name, error)) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s sender '%s' open: %s\n", weblinked::sharedSurfaceProtocol(),
              name.c_str(), surface->describe().c_str());
  std::printf("sending %s at %dx%d for %d s\n", pattern.c_str(), format.width,
              format.height, seconds);
  std::fflush(stdout);

  // 50 Hz by sleep rather than by the engine's clock: this harness is checking
  // the output, not the pacing. Nothing here should be read as evidence about
  // WebLinked's timing on Windows.
  const int ticks = seconds * 50;
  for (int tick = 0; tick < ticks; ++tick) {
    surface->publish(frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  std::printf("published %lld, skipped %lld\n",
              static_cast<long long>(surface->publishedCount()),
              static_cast<long long>(surface->skippedCount()));
  surface->close();
  return 0;
}
