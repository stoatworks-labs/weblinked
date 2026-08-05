// spout_probe — an independent Spout receiver, for verifying the shared output
// on Windows. The counterpart of tools/syphon_probe.mm.
//
// **Less independent than syphon_probe, and the difference matters.**
// syphon_probe links Resolume Arena's *own* Syphon framework, so a pass is two
// vendors' implementations agreeing. Here there is no second implementation to
// hand: this uses spoutDX's receive path against spoutDX's send path, from the
// same SDK. It therefore proves that our backend drives the sender API
// correctly and that the pixels survive the shared texture — not that Spout
// itself is right. A pass here is weaker evidence than a Syphon pass, and
// docs/04-verification.md says so.
//
// The send and receive halves are at least genuinely different code:
// `SendImage` does `UpdateSubresource` onto the shared texture, `ReceiveImage`
// copies to staging textures and maps them back. A channel-order or pitch
// mistake in our backend shows up here.
//
// Build: tools/build_spout_test.ps1

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "SpoutDX.h"

namespace {

struct Sample {
  const char* name;
  int b, g, r, a;
};

/// Exact: unlike the SDI and NDI paths there is no 4:2:2 round trip to forgive
/// anything, and unlike Chromium's premultiply there is no rounding — the
/// harness writes these bytes literally. Anything but an exact match is a real
/// difference.
constexpr int kTolerance = 0;

/// Whether a run of samples matches when the received buffer is read as BGRA,
/// and separately as RGBA. Reported rather than assumed, because `ReceiveImage`
/// decides channel order from the sender's format through `m_bSwapRB` and the
/// answer is worth seeing rather than guessing.
struct Reading {
  bool bgra = true;
  bool rgba = true;
};

bool matches(int got, int want) { return std::abs(got - want) <= kTolerance; }

void sample(const std::vector<unsigned char>& pixels, int width, int x, int y,
            const Sample& want, Reading& reading, bool verbose) {
  const unsigned char* p = pixels.data() + ((static_cast<size_t>(y) * width + x) * 4);
  const bool asBgra = matches(p[0], want.b) && matches(p[1], want.g) &&
                      matches(p[2], want.r) && matches(p[3], want.a);
  // The same expectation with red and blue exchanged.
  const bool asRgba = matches(p[0], want.r) && matches(p[1], want.g) &&
                      matches(p[2], want.b) && matches(p[3], want.a);
  reading.bgra = reading.bgra && asBgra;
  reading.rgba = reading.rgba && asRgba;
  if (verbose) {
    std::printf("  (%4d,%4d) %-12s got %3d %3d %3d %3d  want BGRA %3d %3d %3d %3d  %s\n",
                x, y, want.name, p[0], p[1], p[2], p[3], want.b, want.g, want.r,
                want.a, asBgra ? "ok" : (asRgba ? "ok if RGBA" : "MISMATCH"));
  }
}

int report(const char* what, const Reading& reading) {
  if (reading.bgra) {
    std::printf("%s: PASS (BGRA, as sent)\n", what);
    return 0;
  }
  if (reading.rgba) {
    std::printf("%s: FAIL — pixels arrived with red and blue exchanged.\n", what);
    std::printf("  The sender writes BGRA and spoutDX's default sender format is\n"
                "  DXGI_FORMAT_B8G8R8A8_UNORM, so this means that default moved.\n");
    return 1;
  }
  std::printf("%s: FAIL\n", what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string wanted = "WLTest";
  std::string pattern = "alphabars";
  bool list = false;
  int waitSeconds = 10;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      list = true;
    } else if (arg == "--source" && i + 1 < argc) {
      wanted = argv[++i];
    } else if (arg == "--pattern" && i + 1 < argc) {
      pattern = argv[++i];
    } else if (arg == "--wait" && i + 1 < argc) {
      waitSeconds = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: spout_probe [--list] [--source <name>] "
                   "[--pattern alphabars|updown] [--wait <seconds>]\n");
      return 2;
    }
  }

  spoutDX receiver;
  if (!receiver.OpenDirectX11(nullptr)) {
    std::fprintf(stderr, "could not open a DirectX 11 device\n");
    return 1;
  }

  if (list) {
    const int count = receiver.GetSenderCount();
    std::printf("%d Spout sender(s):\n", count);
    for (int i = 0; i < count; ++i) {
      char name[256] = {0};
      if (receiver.GetSender(i, name, sizeof(name))) {
        std::printf("  %s\n", name);
      }
    }
    if (count == 0) {
      receiver.CloseDirectX11();
      return 1;
    }
  }

  receiver.SetReceiverName(wanted.c_str());

  // The first ReceiveImage calls only negotiate: they discover the sender and
  // raise IsUpdated so the caller can size its buffer. Pixels arrive after
  // that, and only when the sender has produced a new frame — so this loops
  // rather than reading once, the same lesson syphon_probe learned the hard
  // way about -newFrameImage handing back an empty first image.
  std::vector<unsigned char> pixels;
  unsigned int width = 0, height = 0;
  int frames = 0;
  const int attempts = waitSeconds * 100;
  for (int attempt = 0; attempt < attempts && frames < 5; ++attempt) {
    if (receiver.ReceiveImage(pixels.empty() ? nullptr : pixels.data(), width, height)) {
      if (receiver.IsUpdated()) {
        width = receiver.GetSenderWidth();
        height = receiver.GetSenderHeight();
        pixels.assign(static_cast<size_t>(width) * height * 4, 0);
        std::printf("connected to '%s' at %ux%u\n", receiver.GetSenderName(),
                    width, height);
        continue;
      }
      if (!pixels.empty() && receiver.IsConnected()) {
        ++frames;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (pixels.empty() || frames == 0) {
    std::fprintf(stderr, "no frames arrived from '%s'\n", wanted.c_str());
    receiver.ReleaseReceiver();
    receiver.CloseDirectX11();
    return 1;
  }
  std::printf("settled after %d frame(s)\n", frames);

  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  int failures = 0;

  if (pattern == "updown") {
    // Row 0 is the top of the image. Red there means no flip was introduced.
    Reading reading;
    std::printf("orientation:\n");
    sample(pixels, w, w / 2, h / 8, {"top red", 0, 0, 192, 255}, reading, true);
    sample(pixels, w, w / 2, (h * 7) / 8, {"bottom blue", 192, 0, 0, 255}, reading, true);
    failures += report("orientation", reading);
  } else {
    const Sample bands[4] = {
        {"opaque red", 0, 0, 192, 255},
        {"50% green", 0, 96, 0, 128},
        {"25% blue", 48, 0, 0, 64},
        {"transparent", 0, 0, 0, 0},
    };
    Reading reading;
    std::printf("alphabars:\n");
    for (int band = 0; band < 4; ++band) {
      sample(pixels, w, (w * (2 * band + 1)) / 8, h / 2, bands[band], reading, true);
    }
    failures += report("alphabars", reading);
  }

  receiver.ReleaseReceiver();
  receiver.CloseDirectX11();
  return failures == 0 ? 0 : 1;
}
