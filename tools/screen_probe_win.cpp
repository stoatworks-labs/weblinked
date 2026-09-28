// Checks the Windows screen output by looking at the glass, not the counters.
//
// Opens the real WinScreenWindow on display 0, feeds it quadrant colour bars
// (red, green / blue, white), reads the desktop back with BitBlt and checks
// each quadrant, then writes what it saw to capture.bmp. Exit 0 is PASS.
//
// The counters alone cannot catch the failure this was written for: with no
// viewport set, the D3D11 backend presented every refresh and showed black.
//
// Build from a VS x64 developer prompt at the repo root:
//
//   cl /nologo /std:c++17 /EHsc /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I src
//      tools\screen_probe_win.cpp src\outputs\screen_window_win.cpp
//      src\core\frame.cpp src\core\video_format.cpp /Fe:screen_probe.exe
//      /link user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib
//
// Run it on the interactive desktop (Session 1). Over ssh it lands in Session
// 0, which has no display to read back; a scheduled task with an Interactive
// logon puts it on the desktop. Optional argument: seconds to run (default 3).
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>
#include "outputs/screen_window.h"

using namespace weblinked;

int main(int argc, char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  VideoFormat format;
  format.width = 1280;
  format.height = 720;
  VideoFrame frame(format, PixelFormat::kBGRA);
  // BGRA quadrants: TL red, TR green, BL blue, BR white.
  for (int y = 0; y < 720; ++y) {
    uint8_t* row = frame.data() + static_cast<size_t>(y) * frame.rowBytes();
    for (int x = 0; x < 1280; ++x) {
      uint8_t b = 0, g = 0, r = 0;
      const bool right = x >= 640, bottom = y >= 360;
      if (!right && !bottom) r = 255;
      else if (right && !bottom) g = 255;
      else if (!right && bottom) b = 255;
      else r = g = b = 255;
      row[x * 4 + 0] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
    }
  }

  auto displays = enumerateDisplays();
  if (displays.empty()) { std::printf("RESULT no displays\n"); return 2; }
  const auto& d = displays[0];
  std::printf("display 0: %s %dx%d @%.0fHz\n", d.name.c_str(), d.width, d.height, d.refreshHz);

  auto window = createScreenWindow();
  std::string error;
  if (!window->open(format, 0, ScreenScaling::kFit, error)) {
    std::printf("RESULT open failed: %s\n", error.c_str());
    return 2;
  }
  std::printf("renderer: %s\n", window->describe().c_str());
  const int seconds = argc > 1 ? (std::max)(1, std::atoi(argv[1])) : 3;
  for (int i = 0; i < seconds * 25; ++i) {
    window->present(frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (i % 25 == 24) {
      std::printf("t=%2ds presented %lld dropped %lld\n", (i + 1) / 25,
                  (long long)window->presentedCount(), (long long)window->droppedCount());
      std::fflush(stdout);
    }
  }

  // Monitor 0 is the primary, whose origin is (0,0) on the virtual desktop.
  const int W = d.width, H = d.height;
  const double s = (std::min)(double(W) / 1280, double(H) / 720);
  const int fw = int(1280 * s), fh = int(720 * s);
  const int ox = (W - fw) / 2, oy = (H - fh) / 2;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, W, H);
  SelectObject(memory, bitmap);
  BitBlt(memory, 0, 0, W, H, screen, 0, 0, SRCCOPY | CAPTUREBLT);

  struct Probe { const char* name; double fx, fy; COLORREF want; } probes[] = {
      {"top-left", 0.25, 0.25, RGB(255, 0, 0)},
      {"top-right", 0.75, 0.25, RGB(0, 255, 0)},
      {"bottom-left", 0.25, 0.75, RGB(0, 0, 255)},
      {"bottom-right", 0.75, 0.75, RGB(255, 255, 255)},
  };
  int failures = 0;
  for (const auto& p : probes) {
    const int x = ox + int(fw * p.fx), y = oy + int(fh * p.fy);
    const COLORREF got = GetPixel(memory, x, y);
    const bool ok = abs(GetRValue(got) - GetRValue(p.want)) < 16 &&
                    abs(GetGValue(got) - GetGValue(p.want)) < 16 &&
                    abs(GetBValue(got) - GetBValue(p.want)) < 16;
    failures += ok ? 0 : 1;
    std::printf("%-12s (%4d,%4d) got %3d %3d %3d  want %3d %3d %3d  %s\n", p.name, x, y,
                GetRValue(got), GetGValue(got), GetBValue(got), GetRValue(p.want),
                GetGValue(p.want), GetBValue(p.want), ok ? "ok" : "WRONG");
  }

  // Keep a picture of what was on the glass.
  BITMAPINFOHEADER header{};
  header.biSize = sizeof(header);
  header.biWidth = W;
  header.biHeight = H;
  header.biPlanes = 1;
  header.biBitCount = 24;
  const int stride = ((W * 3 + 3) / 4) * 4;
  std::vector<uint8_t> pixels(size_t(stride) * H);
  GetDIBits(memory, bitmap, 0, H, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header),
            DIB_RGB_COLORS);
  BITMAPFILEHEADER file{};
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(file) + sizeof(header);
  file.bfSize = file.bfOffBits + DWORD(pixels.size());
  if (FILE* f = std::fopen("capture.bmp", "wb")) {
    std::fwrite(&file, sizeof(file), 1, f);
    std::fwrite(&header, sizeof(header), 1, f);
    std::fwrite(pixels.data(), pixels.size(), 1, f);
    std::fclose(f);
  }

  std::printf("presented %lld dropped %lld\n", (long long)window->presentedCount(),
              (long long)window->droppedCount());
  window->close();
  std::printf("RESULT %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
