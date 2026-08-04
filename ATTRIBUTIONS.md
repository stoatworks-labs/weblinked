# Attributions

WebLinked is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Chromium Embedded Framework

<https://bitbucket.org/chromiumembedded/cef>  
Licence: BSD-3-Clause  
Copyright: Marshall A. Greenblatt, with portions copyright Google Inc.

Binary distribution vendored under third_party/cef/.

The browser engine. WebLinked's entire premise is rendering a real web page to a video output, which means embedding a real browser.

### Spout

<https://github.com/leadedge/Spout2>  
Licence: BSD-2-Clause  
Copyright: Lynn Jarvis

Vendored under third_party/spout/.

Shares a GPU texture with other Windows applications without a round trip through system memory.

### Syphon Framework

<https://github.com/Syphon/Syphon-Framework>  
Licence: BSD-3-Clause  
Copyright: Tom Butterworth (bangnoise) and Anton Marini (vade)

Vendored under third_party/syphon/.

The macOS counterpart to Spout — same job, same reason.

### miniaudio

<https://github.com/mackron/miniaudio>  
Licence: Public Domain (Unlicense) or MIT-0, at your choice  
Copyright: David Reid

Single header vendored at third_party/miniaudio/miniaudio.h, unmodified.

In the tree rather than fetched because it is the only thing between the app and a sound card, and a build should not need the network on a locked-down show LAN.

### NDI SDK

<https://ndi.video/for-developers/ndi-sdk/>  
Licence: NDI SDK Licence Agreement (proprietary)  
Copyright: Vizrt Group

Headers only, vendored so the backend compiles everywhere. The runtime is never redistributed — it is loaded with dlopen at run time if the user has installed it.

NDI is the video transport most of this fleet's users already run. Compiling against the headers without shipping the runtime keeps the licence intact and still gives every build the backend.

### Open Media Transport (libomt)

<https://github.com/openmediatransport/libomtnet>  
Licence: MIT  
Copyright: Open Media Transport Contributors

Header vendored alongside the NDI headers; the library is loaded at run time.

The open alternative to NDI, and MIT end to end — so unlike NDI it can be supported without a proprietary licence in the path.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
