#pragma once

namespace weblinked {

/// One tick's worth of audio, offered in both layouts.
///
/// The engine prepares planar and interleaved once per tick rather than making
/// each backend convert: NDI and OMT want planar float, DeckLink and AJA want
/// interleaved integers, and doing it per-output would mean up to four copies
/// of the same 1920 samples.
///
/// This lives in `core` rather than beside IOutput because the router that
/// gains, maps and delays it has to be testable in a build with no CEF and no
/// vendor SDK in it — which is the whole point of the core/engine split.
struct AudioBlock {
  const float* const* planes = nullptr;  ///< [channel][frame]
  const float* interleaved = nullptr;    ///< frame-major, channels interleaved
  int frames = 0;
  int channels = 0;
  int sampleRate = 0;

  bool valid() const { return frames > 0 && channels > 0 && sampleRate > 0; }
};

}  // namespace weblinked
