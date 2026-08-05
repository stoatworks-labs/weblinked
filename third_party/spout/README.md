# Spout (vendored, DirectX sender subset)

From <https://github.com/leadedge/Spout2>, BSD 2-clause — see `LICENSE.txt`,
which is upstream's `LICENSE` unmodified. That licence requires the copyright
notice be retained, which is why every file here still carries its original
header and why none of them have been edited.

Used by `src/outputs/shared_surface_win.cpp`. The macOS counterpart is
`third_party/syphon`, and the two READMEs are deliberately parallel.

## Why the source and not the DLL

Spout ships prebuilt `SpoutDX.dll` binaries. Vendoring the sources instead
means no redistributable to install, nothing to find at run time, and nothing
extra to sign — the sender is inside the executable. BSD-2 permits it and MIT
is compatible with it, unlike the NDI SDK; see the long note at the head of
`src/outputs/ndi_output.cpp` for the case where that is *not* true and why NDI
is `dlopen`ed instead.

## Why only some of it

This build is a **sender** that already holds its pixels on the CPU, because
that is what CEF's `OnPaint` gives us. `spoutDX` is the DirectX-only class:
no OpenGL context, no window, no message pump. So none of the OpenGL half is
here — `Spout.cpp`, `SpoutGL.cpp`, `SpoutGLextensions.cpp`, `SpoutSender.cpp`,
`SpoutReceiver.cpp` — and neither is anything for receiving.

What is here is the closure of `spoutDX`:

```
SpoutDX             the sender: OpenDirectX11, SetSenderName, SendImage
SpoutDirectX        device and shared-texture creation
SpoutSenderNames    the shared-memory registry a receiver reads names from
SpoutFrameCount     the frame-sync mutex and new-frame signal
SpoutSharedMemory   the shared-memory primitive underneath both of those
SpoutCopy           pixel copy and format helpers
SpoutUtils          logging, registry, version helpers
SpoutCommon.h       the export macros and shared defines
```

`SpoutCopy.h` includes `<GL/gl.h>` for enum definitions only. That header is
part of the Windows SDK and no OpenGL is linked or called.

## Layout, and why it is this one

Everything sits flat in `Spout/`, which is upstream's own supported
arrangement rather than a liberty taken here: `SpoutDX.h` selects its include
prefix with `#if __has_include("SpoutCommon.h")`, taking the same-folder branch
when the files are together and the `../../SpoutGL/` branch when they are laid
out as in the repository. Flat means the first branch, and no file needed
editing.

## Build settings

Set per-file in the top-level `CMakeLists.txt`:

* **`/w`**. Upstream builds cleanly at its own warning level, not at the one
  CEF imposes on this target.

`psapi.lib` links itself through a `#pragma comment(lib, "psapi.lib")` inside
`SpoutDX.h`; `d3d11` and `dxgi` are linked by the target.

## Re-vendoring

Copy the file list above from a fresh checkout — `SpoutDX.{h,cpp}` from
`SPOUTSDK/SpoutDirectX/SpoutDX/`, the rest from `SPOUTSDK/SpoutGL/` — keep
`LICENSE` as `LICENSE.txt`, and change nothing else.

Taken from upstream `main`, 2026-08-04.

## One thing to know before trusting it

`spoutDX`'s default sender format is `DXGI_FORMAT_B8G8R8A8_UNORM`, which is
exactly `PixelFormat::kBGRA`, and `SendImage` passes our `rowBytes` straight to
`UpdateSubresource` as the pitch. There is therefore no conversion and no flip
anywhere in the Windows path. If a future Spout changes that default, the
symptom will be swapped red and blue channels rather than a build failure.
