// dl_profile — what profile is the card in, and what can each sub-device do.
//
// Section 19 of docs/04-verification.md published a table like this for a
// two-sub-device full-duplex profile. The card can be reprofiled, which
// silently invalidates that table, so measure it rather than trust it.
//
// Build (adjust the SDK path):
//   SDK="/path/to/Blackmagic DeckLink SDK/Mac/include"
//   clang++ -std=c++17 -fobjc-arc -I"$SDK" "$SDK/DeckLinkAPIDispatch.cpp" \
//     tools/dl_profile.mm -framework CoreFoundation -o dl_profile
#include "DeckLinkAPI.h"
#include <cstdio>
#include <string>

static const char* duplexName(int64_t d) {
  switch ((uint32_t)d) {
    case bmdDuplexFull:     return "full duplex";
    case bmdDuplexHalf:     return "half duplex";
    case bmdDuplexSimplex:  return "simplex";
    case bmdDuplexInactive: return "INACTIVE";
    default:                return "?";
  }
}
static const char* profileName(int64_t p) {
  switch ((uint32_t)p) {
    case bmdProfileOneSubDeviceFullDuplex:   return "1 sub-device full duplex";
    case bmdProfileOneSubDeviceHalfDuplex:   return "1 sub-device half duplex";
    case bmdProfileTwoSubDevicesFullDuplex:  return "2 sub-devices full duplex";
    case bmdProfileTwoSubDevicesHalfDuplex:  return "2 sub-devices half duplex";
    case bmdProfileFourSubDevicesHalfDuplex: return "4 sub-devices half duplex";
    default:                                 return "?";
  }
}

int main() {
  IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
  if (!it) { std::printf("no DeckLink API\n"); return 2; }
  IDeckLink* d = nullptr; int i = 0;
  while (it->Next(&d) == S_OK) {
    CFStringRef nm = nullptr; d->GetDisplayName(&nm); char nb[128] = {0};
    if (nm) { CFStringGetCString(nm, nb, sizeof(nb), kCFStringEncodingUTF8); CFRelease(nm); }

    int64_t duplex = 0, profileId = 0, videoIO = 0, subCount = 0, subIndex = 0;
    bool extKey = false, intKey = false;
    IDeckLinkProfileAttributes* a = nullptr;
    if (d->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&a) == S_OK) {
      a->GetInt(BMDDeckLinkDuplex, &duplex);
      a->GetInt(BMDDeckLinkProfileID, &profileId);
      a->GetInt(BMDDeckLinkVideoIOSupport, &videoIO);
      a->GetInt(BMDDeckLinkNumberOfSubDevices, &subCount);
      a->GetInt(BMDDeckLinkSubDeviceIndex, &subIndex);
      a->GetFlag(BMDDeckLinkSupportsExternalKeying, &extKey);
      a->GetFlag(BMDDeckLinkSupportsInternalKeying, &intKey);
      a->Release();
    }

    // Ask the hardware directly whether it would accept the rasters we care
    // about, rather than inferring from the profile.
    IDeckLinkOutput* out = nullptr;
    std::string modes;
    if (d->QueryInterface(IID_IDeckLinkOutput, (void**)&out) == S_OK) {
      struct M { const char* name; BMDDisplayMode m; BMDPixelFormat p; };
      const M probes[] = {
        {"1080p50/YUV",  bmdModeHD1080p50, bmdFormat8BitYUV},
        {"1080p25/YUV",  bmdModeHD1080p25, bmdFormat8BitYUV},
        {"1080p50/BGRA", bmdModeHD1080p50, bmdFormat8BitBGRA},
        {"1080p25/BGRA", bmdModeHD1080p25, bmdFormat8BitBGRA},
      };
      for (const M& p : probes) {
        bool ok = false;
        out->DoesSupportVideoMode(bmdVideoConnectionUnspecified, p.m, p.p,
                                  bmdNoVideoOutputConversion,
                                  bmdSupportedVideoModeDefault, nullptr, &ok);
        modes += std::string(p.name) + "=" + (ok ? "yes" : "no") + "  ";
      }
      out->Release();
    } else {
      modes = "(no output interface)";
    }

    const bool hasIn  = (videoIO & bmdDeviceSupportsCapture) != 0;
    const bool hasOut = (videoIO & bmdDeviceSupportsPlayback) != 0;

    std::printf("index %d  %-18s  %-12s  in:%-3s out:%-3s  ext key:%-3s int key:%-3s\n",
                i, nb, duplexName(duplex), hasIn ? "yes" : "no", hasOut ? "yes" : "no",
                extKey ? "yes" : "no", intKey ? "yes" : "no");
    std::printf("           profile: %s   sub-device %lld of %lld\n",
                profileName(profileId), (long long)subIndex, (long long)subCount);
    std::printf("           output modes: %s\n\n", modes.c_str());

    d->Release(); ++i;
  }
  it->Release();
  if (i == 0) std::printf("no DeckLink devices\n");
  return 0;
}
