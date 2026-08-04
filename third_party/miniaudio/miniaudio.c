/* The one translation unit that compiles miniaudio.
 *
 * Kept as its own C file, and its own CMake target, for two reasons. The
 * obvious one is that a 4 MB header compiled into more than one translation
 * unit is a build-time tax for nothing. The one that actually matters is that
 * everything in src/engine is built with CEF's flags — -fno-exceptions,
 * -fno-rtti, -Werror — and miniaudio is third-party C that has no business
 * being held to our warning settings, or being compiled by a C++ compiler at
 * all.
 *
 * The MA_NO_* list below is not tuning. WebLinked needs exactly one thing from
 * miniaudio: an f32 playback device with a callback. Everything else it can do
 * — decoding MP3s, resource management, its node graph and high-level engine —
 * is a dependency we would be shipping and never calling.
 */

/* Playback only, and only the parts of it we use. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

/* Backends. The defaults are close to right; these are the ones worth being
 * deliberate about.
 *
 * Windows keeps WASAPI and drops DirectSound and WinMM: both are legacy paths
 * that would silently be chosen ahead of nothing at all, and a show running on
 * DirectSound without anyone having asked for it is worse than a device that
 * refuses to open and says why.
 *
 * ASIO is absent because Steinberg's SDK cannot be redistributed under this
 * repo's licence. That is a licensing fact, not an oversight — see
 * docs/07-audio.md.
 */
#if defined(_WIN32)
    #define MA_NO_DSOUND
    #define MA_NO_WINMM
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
