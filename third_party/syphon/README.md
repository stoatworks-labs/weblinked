# Syphon (vendored, server subset)

From <https://github.com/Syphon/Syphon-Framework>, **BSD 3-clause** — see
`LICENSE.txt`, which is upstream's `License.txt` unmodified. That licence
requires the copyright notice be retained, which is why every file here still
carries its original header and why none of them have been edited. The third
clause forbids using the authors' names to endorse or promote anything derived
from it, so do not put Syphon's authors in WebLinked's marketing.

Worth stating because it is an easy mistake: the copy Resolume Arena ships at
`/Applications/Resolume Arena/Licenses/syphon.md` carries only two clauses.
Upstream's has three, and upstream's is the one governing what is vendored
here. Spout, next door, genuinely is BSD-2.

Used by `src/outputs/shared_surface_mac.mm`.

## Why the source and not the framework

Syphon ships as `Syphon.framework`. Vendoring the sources instead buys two
things:

* **No nested binary to sign and notarise.** A framework inside the app bundle
  is another thing for `SignMacBundle.cmake` to get right, and approving an app
  does not unquarantine its nested binaries.
* **BSD-3 permits it and MIT is compatible with it**, unlike the NDI SDK — see
  the long note at the head of `src/outputs/ndi_output.cpp` for the case where
  that is *not* true and why NDI is therefore `dlopen`ed instead.

## Why only some of it

This build is a **server** that already holds its pixels on the CPU, because
that is what CEF's `OnPaint` gives us. It needs the announcement protocol and
the IOSurface plumbing, and nothing else. Specifically it does not need:

* the OpenGL and Metal servers, renderers, shaders and vertex helpers — we
  write the surface directly rather than rendering a texture into it;
* any client class — `SyphonClient`, `SyphonOpenGLClient`, `SyphonMetalClient`,
  `SyphonImage`, `SyphonServerDirectory`. Consumers are other applications.

What is here is the closure of `SyphonServerBase` plus the messaging layer:

```
SyphonServerBase              the server, and the announce/retire broadcasts
SyphonServerConnectionManager tracks attached clients, owns the surface ID
SyphonPrivate                 the description keys and notification names
SyphonMessaging               \
SyphonMessageQueue             |
SyphonMessageSender            |  the CFMessagePort transport between
SyphonMessageReceiver          |  server and client
SyphonCFMessageSender          |
SyphonCFMessageReceiver        |
SyphonDispatch                /
```

`SyphonSubclassing.h` is the header exposing `newSurfaceForWidth:height:options:`
and `publish`, which is the entire API this backend uses. `SyphonClientBase.h`
is here only because `SyphonSubclassing.h` imports it to declare a category —
no client is compiled or linked.

## Layout, and why it is this one

Headers and sources sit together in `Syphon/`, and `third_party/syphon` is the
include root. That makes both import styles resolve against unmodified upstream
files: quoted imports (`#import "SyphonPrivate.h"`) find their neighbour, and
`SyphonSubclassing.h`'s framework-style `#import <Syphon/SyphonServerBase.h>`
finds it through the include path.

## Build settings that are not optional

Set per-file in the top-level `CMakeLists.txt`, because none of them are true
of anything else in this repository:

* **`-fobjc-arc`** on the `.m` files. Syphon 6 is ARC (`CLANG_ENABLE_OBJC_ARC`
  in its Xcode project) and will not compile without it. `SyphonDispatch.c` is
  plain C and must *not* get the flag.
* **`-include Syphon_Prefix.pch`**. That prefix header is where `SYPHONLOG` is
  defined; the `.m` files do not compile without it.
* **`-w`**. Upstream uses `kIOSurfaceIsGlobal`, deprecated since 10.11 with no
  replacement — it is how Syphon shares a surface between processes at all.

## Re-vendoring

Copy the file list above from a fresh checkout, keep `License.txt` as
`LICENSE.txt`, and change nothing else. If upstream gains a server API that
publishes an IOSurface directly, this subset can shrink further.

Taken from upstream `main`, 2026-08-04. Protocol verified compatible with the
Syphon 5 client that Resolume Arena bundles: identical description keys,
notification names and `SyphonSurfaceTypeIOSurface`.
