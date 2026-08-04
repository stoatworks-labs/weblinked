# miniaudio

Vendored, not fetched. `miniaudio.h` v0.11.25 (2026-03-04), from
<https://github.com/mackron/miniaudio>, unmodified.

It is in the tree rather than downloaded at configure time because it is the
only thing standing between WebLinked and a sound card: a build that needs the
network to produce an audio output would fail on exactly the machines — a
locked-down show network, a venue with no uplink — where you most want to be
able to rebuild.

## Licence

Dual: public domain (Unlicense) **or** MIT-0, at your choice. Both are
compatible with this repo's MIT licence, and neither requires attribution in a
binary. The full text of both is at the end of `miniaudio.h`, which is why
there is no separate LICENSE file here — copying it out would just create a
second copy to go stale.

## What we compile

`miniaudio.c` is the single implementation translation unit, with the high-level
half of the library switched off — see the comment at the top of it. Backends
in use:

| Platform | Backend |
|---|---|
| macOS | Core Audio |
| Windows | WASAPI (shared and exclusive) |
| Linux | PulseAudio, ALSA, JACK — whichever answers first |

**No ASIO.** miniaudio can drive ASIO, but only against Steinberg's SDK, whose
licence forbids redistributing the headers. Shipping it would mean either
breaking that licence or making every Windows build depend on an SDK the user
has to register for. WASAPI in exclusive mode reaches single-digit-millisecond
latency on the same interfaces, which for a playout device driving SDI is well
inside what matters. If you need ASIO specifically, `docs/07-audio.md` describes
where it would slot in.

## Updating

Replace `miniaudio.h`, update the version line above, and rebuild clean. The
project uses only `ma_context`, `ma_device` and device enumeration — a very
stable corner of the API — but it is worth re-reading `src/outputs/audio_device.cpp`
against the new header's device-callback contract before trusting it on a show.
