# NDI distribution, licensing and attribution

Why WebLinked loads NDI the way it does, what the licence actually permits, and
what any repository in this fleet has to say when it mentions NDI.

This is the reference copy. Other projects link here rather than restating it.

## The licence permits redistribution — conditionally

The common belief that NDI cannot be shipped with an application is wrong. The
standard SDK is **royalty-free**, and Vizrt's own
[Software Distribution](https://docs.ndi.video/all/developing-with-ndi/sdk/software-distribution)
page grants two distinct rights:

1. **Ship the libraries inside your application.** Permitted, provided the
   libraries live in your application's own folder and never in a system path.
2. **Bundle the redistributable installer.** Permitted, including silent
   installation, provided you keep the bundled version current.

Both carry the same condition: the licence under which *you* distribute must
contain terms that prohibit modifying, reverse-engineering, disassembling or
decompiling the SDK.

## Why this repository still uses dlopen

That condition is the whole reason. WebLinked is MIT, and MIT grants users the
right to modify and redistribute everything in the tree. It cannot
simultaneously forbid those things for `libndi.dylib`. Committing NDI binaries
here, or attaching them to a GitHub release, would contradict our own LICENSE.

So the source tree carries **only the headers** — which Vizrt explicitly permits
open-source projects to distribute under MIT terms — and resolves the library at
run time. See the comment at the top of
[`src/outputs/ndi_output.cpp`](../src/outputs/ndi_output.cpp).

The second reason is practical and unrelated to licensing: the flat C entry
points (`NDIlib_send_create` and friends) exist in every NDI 5 and 6 runtime,
whereas `NDIlib_v6_load()` returns a versioned struct whose layout changes
between SDK generations. Resolving flat symbols means one binary accepts either
runtime.

### What that buys the operator

A single build works whether the machine has the full SDK, the redistributable
runtime, or nothing at all. With nothing, the NDI output reports itself
unavailable and the application still launches — the browser, preview and every
other backend keep working.

## The search order

`NdiRuntime::acquire` tries, in order:

1. `$NDI_RUNTIME_DIR_V6` (`NDILIB_REDIST_FOLDER`) — set by the redistributable
   installer, and the documented way to find a runtime off the default path
2. Next to the executable, then `Contents/Frameworks` inside a macOS bundle
3. The platform's own loader search path
4. Known SDK install locations (`/Library/NDI SDK for Apple/lib/macOS`,
   `/usr/local/lib`, `/opt/homebrew/lib`, `/usr/lib`)

Position 2 is what makes the installer route below work without any code change.

## When the runtime is missing

The failure message names the library, gives the **download URL**, and names the
environment variable:

```
NDI runtime not found (libndi.dylib). Install the NDI runtime from
http://ndi.link/NDIRedistV6Apple, or set NDI_RUNTIME_DIR_V6 to the directory
containing it.
```

The URL comes from `NDILIB_REDIST_URL` in the SDK headers, which is
platform-specific and **empty on Linux** — there is no one-click redistributable
there. `redistUrl()` falls back to the SDK download page in that case so the
operator never sees a bare `""`.

In the control page this becomes a clickable link: `errorHtml()` in
[`src/control/web_assets.h`](../src/control/web_assets.h) escapes the message and
then turns any `http(s)` URL in it into an anchor. It escapes first because
output errors can embed an operator-supplied source or device name.

## Shipping the runtime in an installer

A signed installer is not the git tree. It has its own EULA, so it *can* carry
NDI terms forward, and both distribution routes reopen:

- **macOS** — place `libndi.dylib` in `WebLinked.app/Contents/Frameworks`.
  Search-path position 2 finds it with no code change. Sign it inside-out with
  everything else (see `cmake/SignMacBundle.cmake`).
- **Windows** — either drop `Processing.NDI.Lib.x64.dll` beside the `.exe`, or
  bundle the redistributable installer and run it with `/verysilent`.
- **Linux** — no redistributable exists. Ship the `.so` beside the binary, or
  direct the operator to the SDK download.

The installer's EULA must prohibit modifying, reverse-engineering, disassembling
and decompiling the SDK, and the bundled version must be kept current.

## Attribution, every time

Required wherever NDI is mentioned — README, docs, website, About boxes:

- **`NDI® is a registered trademark of Vizrt NDI AB.`** on the same page as
  first use, or in a footnote
- a link to <https://ndi.video> near the mention
- **never redistribute NDI Tools** — link to <https://ndi.video/tools> instead
- using "NDI" *in a product name* needs written permission from Vizrt, which we
  have not sought and do not rely on

## Codec licensing is ours, not Vizrt's

The SDK grant does not cover **H.264, H.265 or AAC**. Those are separately
licensable formats and the obligation sits with whoever ships the product.
WebLinked's NDI output sends uncompressed frames, so this does not currently
bite — but it would the moment an NDI|HX path is added.

## The Advanced SDK

Free to develop against, but a commercial product needs a vendor ID from
`sdk@ndi.video`. It buys compressed passthrough and NDI|HX encode. Its SpeedHQ
acceleration is delivered as FPGA IP cores, so it does nothing for a plain CPU
or SoC target. Not used here.

## OMT, for contrast

[Open Media Transport](https://openmediatransport.org) is MIT end to end —
`libomtnet`, the `libomt` C wrapper and the `libvmx` codec. No conditional
grant, no EULA to carry forward, so it can simply live in the tree; the header
already does, at `third_party/omt/libomt.h`.

Adoption is the trade-off: vMix, an OBS plugin, Nimble Streamer and a pending
TouchDesigner request, against NDI's ubiquity in cameras and playout. WebLinked
ships both and lets the operator choose.
