// dl_scan — which DeckLink inputs are actually receiving anything.
//
// Companion to sdi_probe. Where that one checks colour, this one answers "is
// the signal even arriving, and on which sub-device" — which is the question
// you have when a loopback is dark and you are not certain what is patched to
// what. It counts frames flagged bmdFrameHasNoInputSource *separately* from
// good ones, so these are distinguishable:
//
//   frames=N no-source=0        a real, locked signal
//   frames=0 no-source=N        input alive, nothing plugged in
//   frames~N no-source=few      no lock; a free-running input with nothing on
//                               it reports mode 'ntsc' and a mix of the two
//   EnableVideoInput failed     sub-device inactive in the current profile
//
// Build (adjust the SDK path):
//   SDK="/path/to/Blackmagic DeckLink SDK/Mac/include"
//   clang++ -std=c++17 -fobjc-arc -I"$SDK" "$SDK/DeckLinkAPIDispatch.cpp" \
//     tools/dl_scan.mm -framework CoreFoundation -o dl_scan
#include "DeckLinkAPI.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <unistd.h>

struct Counters { std::atomic<int> good{0}, nosrc{0}; std::atomic<long long> mode{0}; };

class Cb : public IDeckLinkInputCallback {
 public:
  explicit Cb(Counters* c, IDeckLinkInput* in) : c_(c), in_(in) {}
  HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return 1; } ULONG Release() override { return 1; }
  HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                  IDeckLinkDisplayMode* dm,
                                  BMDDetectedVideoInputFormatFlags) override {
    if (dm) {
      c_->mode = (long long)dm->GetDisplayMode();
      in_->PauseStreams();
      in_->EnableVideoInput(dm->GetDisplayMode(), bmdFormat8BitYUV,
                            bmdVideoInputEnableFormatDetection);
      in_->FlushStreams(); in_->StartStreams();
    }
    return S_OK;
  }
  HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* v, IDeckLinkAudioInputPacket*) override {
    if (!v) return S_OK;
    if (v->GetFlags() & bmdFrameHasNoInputSource) ++c_->nosrc; else ++c_->good;
    return S_OK;
  }
 private:
  Counters* c_; IDeckLinkInput* in_;
};

int main() {
  IDeckLinkIterator* it = CreateDeckLinkIteratorInstance(); if (!it) return 2;
  IDeckLink* devs[8]; int n = 0; IDeckLink* d = nullptr;
  while (it->Next(&d) == S_OK && n < 8) devs[n++] = d;
  it->Release();

  for (int i = 0; i < n; ++i) {
    CFStringRef nm = nullptr; devs[i]->GetDisplayName(&nm); char nb[128] = {0};
    if (nm) { CFStringGetCString(nm, nb, sizeof(nb), kCFStringEncodingUTF8); CFRelease(nm); }
    IDeckLinkInput* in = nullptr;
    if (devs[i]->QueryInterface(IID_IDeckLinkInput, (void**)&in) != S_OK) {
      std::printf("index %d %-18s : no input interface\n", i, nb); continue;
    }
    IDeckLinkProfileAttributes* a = nullptr; bool det = false;
    if (devs[i]->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&a) == S_OK) {
      a->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &det); a->Release();
    }
    Counters c; Cb cb(&c, in);
    in->SetCallback(&cb);
    HRESULT hr = in->EnableVideoInput(bmdModeHD1080p25, bmdFormat8BitYUV,
                   det ? bmdVideoInputEnableFormatDetection : bmdVideoInputFlagDefault);
    if (hr != S_OK) {
      std::printf("index %d %-18s : EnableVideoInput failed (0x%x)\n", i, nb, (unsigned)hr);
      in->SetCallback(nullptr); in->Release(); continue;
    }
    if (in->StartStreams() != S_OK) {
      std::printf("index %d %-18s : StartStreams failed\n", i, nb);
      in->DisableVideoInput(); in->SetCallback(nullptr); in->Release(); continue;
    }
    sleep(3);
    in->StopStreams(); in->DisableVideoInput(); in->SetCallback(nullptr);
    std::printf("index %d %-18s : detect=%s  frames=%d  no-source=%d  detected-mode=0x%llx\n",
                i, nb, det ? "yes" : "no", c.good.load(), c.nosrc.load(),
                (unsigned long long)c.mode.load());
    in->Release();
  }
  for (int i = 0; i < n; ++i) devs[i]->Release();
  return 0;
}
