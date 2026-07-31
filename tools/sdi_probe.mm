// sdi_probe — an independent SDI receiver, the DeckLink twin of ndi_probe.
//
// Captures on a DeckLink input and checks the colour bars against a BT.709
// reference written here rather than shared with WebLinked, so this is a check
// on the round trip and not a restatement of it. Proves what came back off the
// wire, not what the application believes it sent.
//
// Build (adjust the SDK path):
//   SDK="/path/to/Blackmagic DeckLink SDK/Mac/include"
//   clang++ -std=c++17 -fobjc-arc -I"$SDK" "$SDK/DeckLinkAPIDispatch.cpp" \
//     tools/sdi_probe.mm -framework CoreFoundation -o sdi_probe
//
// Usage:
//   ./sdi_probe [index] [1080p25] [bands]
//     index    device to capture on (default 1)
//     1080p25  capture at 1080p25 instead of 1080p50
//     bands    check the four alpha bands instead of eight colour bars
//
// On a Duo 2 in full-duplex the SAME sub-device owns connectors 1 and 3, so a
// cable from 1 to 3 loops index 0 back to itself. Enabling external keying
// turns connector 3 into the key output and the capture goes dark — that is
// key + fill working, not a fault.
#include "DeckLinkAPI.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {
struct Ycbcr { int y, cb, cr; };

Ycbcr sampleUyvy(const uint8_t* data, int stride, int x, int y) {
  const uint8_t* row = data + (size_t)y * stride;
  const uint8_t* g = row + (size_t)(x / 2) * 4;
  return {g[(x % 2) == 0 ? 1 : 3], g[0], g[2]};
}
Ycbcr referenceBt709(int r, int g, int b) {
  const double rn = r/255.0, gn = g/255.0, bn = b/255.0;
  const double yn = 0.2126*rn + 0.7152*gn + 0.0722*bn;
  return {(int)std::lround(16.0 + 219.0*yn),
          (int)std::lround(128.0 + 224.0*((bn-yn)/(2.0*(1.0-0.0722)))),
          (int)std::lround(128.0 + 224.0*((rn-yn)/(2.0*(1.0-0.2126))))};
}

std::atomic<int> g_frames{0};
std::atomic<int> g_failures{-1};
std::atomic<bool> g_done{false};
bool g_alphaBands = false;
int g_width = 0, g_height = 0;

class Capture : public IDeckLinkInputCallback {
 public:
  HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return 1; }
  ULONG Release() override { return 1; }
  HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                  IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags) override {
    return S_OK;
  }
  HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* v, IDeckLinkAudioInputPacket*) override {
    if (!v) return S_OK;
    if (v->GetFlags() & bmdFrameHasNoInputSource) return S_OK;
    const int n = ++g_frames;
    // Check on a settled frame, not the first one off the wire.
    if (n == 25) {
      void* bytes = nullptr;
      if (v->GetBytes(&bytes) == S_OK && bytes) {
        g_width = v->GetWidth(); g_height = v->GetHeight();
        const int stride = v->GetRowBytes();
        struct Bar { const char* name; int r, g, b; };
        static const Bar kBars8[] = {
          {"grey",0xc0,0xc0,0xc0},{"yellow",0xc0,0xc0,0x00},{"cyan",0x00,0xc0,0xc0},
          {"green",0x00,0xc0,0x00},{"magenta",0xc0,0x00,0xc0},{"red",0xc0,0x00,0x00},
          {"blue",0x00,0x00,0xc0},{"black",0x00,0x00,0x00}};
        // The fill of an externally keyed signal carries the STRAIGHT colour,
        // so a 50%-alpha green must read as full green - not as the darker
        // premultiplied value. That is the bug section 7 found on NDI.
        static const Bar kBands[] = {
          {"red a=1.0",0xc0,0x00,0x00},{"green a=0.5",0x00,0xc0,0x00},
          {"blue a=0.25",0x00,0x00,0xc0},{"empty a=0",0x00,0x00,0x00}};
        const Bar* kBars = g_alphaBands ? kBands : kBars8;
        const int nbars = g_alphaBands ? 4 : 8;
        const int y = g_height / 4, bw = g_width / nbars;
        int fails = 0;
        std::printf("\n  bar        sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict\n");
        for (int i = 0; i < nbars; ++i) {
          const Ycbcr got = sampleUyvy((const uint8_t*)bytes, stride, i*bw + bw/2, y);
          const Ycbcr want = referenceBt709(kBars[i].r, kBars[i].g, kBars[i].b);
          const bool ok = std::abs(got.y-want.y)<=3 && std::abs(got.cb-want.cb)<=3 &&
                          std::abs(got.cr-want.cr)<=3;
          if (!ok) ++fails;
          std::printf("  %-12s  %4d %4d %4d      %4d %4d %4d       %s\n", kBars[i].name,
                      got.y,got.cb,got.cr, want.y,want.cb,want.cr, ok?"ok":"MISMATCH");
        }
        g_failures = fails;
      }
      g_done = true;
    }
    return S_OK;
  }
};
}  // namespace

int main(int argc, char** argv) {
  const int index = argc > 1 ? std::atoi(argv[1]) : 1;
  const bool p25 = argc > 2 && std::string(argv[2]) == "1080p25";
  const bool alphaBands = argc > 3 && std::string(argv[3]) == "bands";
  g_alphaBands = alphaBands;
  IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
  if (!it) { std::printf("no DeckLink API\n"); return 2; }
  IDeckLink* d = nullptr; int i = 0; IDeckLink* chosen = nullptr;
  while (it->Next(&d) == S_OK) { if (i++ == index) { chosen = d; break; } d->Release(); }
  it->Release();
  if (!chosen) { std::printf("no device at index %d\n", index); return 2; }
  CFStringRef nm = nullptr; chosen->GetDisplayName(&nm);
  char nb[256] = {0}; if (nm) { CFStringGetCString(nm, nb, sizeof(nb), kCFStringEncodingUTF8); CFRelease(nm); }
  std::printf("capturing on index %d: %s\n", index, nb);

  IDeckLinkInput* in = nullptr;
  if (chosen->QueryInterface(IID_IDeckLinkInput, (void**)&in) != S_OK) {
    std::printf("device has no input interface\n"); return 2;
  }
  Capture cap;
  in->SetCallback(&cap);
  if (in->EnableVideoInput(p25 ? bmdModeHD1080p25 : bmdModeHD1080p50, bmdFormat8BitYUV, bmdVideoInputFlagDefault) != S_OK) {
    std::printf("EnableVideoInput failed\n"); return 2;
  }
  if (in->StartStreams() != S_OK) { std::printf("StartStreams failed\n"); return 2; }

  for (int s = 0; s < 100 && !g_done; ++s) usleep(100000);
  in->StopStreams(); in->DisableVideoInput(); in->SetCallback(nullptr);
  in->Release(); chosen->Release();

  std::printf("\nframes received: %d", g_frames.load());
  if (g_width) std::printf("  (%dx%d)", g_width, g_height);
  std::printf("\n");
  if (g_frames == 0) { std::printf("NO SIGNAL on this input\n"); return 1; }
  if (g_failures < 0) { std::printf("signal present but no frame checked\n"); return 1; }
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
