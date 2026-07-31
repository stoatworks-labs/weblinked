# NDI SDK headers (vendored)

`include/` is a verbatim copy of the header files from the NDI SDK
(6.3.2, macOS). Nothing else from the SDK is here — **no libraries, and no
redistributable runtime.**

## Why they are here

WebLinked loads `libndi` with `dlopen` at run time, so it never links the SDK.
But it still has to *compile* against these headers, and `find_package(NDI)`
previously required a full SDK installation to find them. Hosted CI runners have
no SDK, so `find_package` quietly disabled the backend and **every released
binary shipped with no NDI support at all** — the SDI/IP tool's most-used output,
missing from the artefacts people actually download.

Vendoring the headers removes that: the NDI backend now compiles everywhere, and
whether it *works* is decided at run time by whether a runtime is present, which
is the right place for that question.

## The licence permits this explicitly

Every one of these files opens with:

> NOTE : The following MIT license applies to this file ONLY and not to the SDK
> as a whole.

followed by a full MIT grant including the right to distribute and sublicense.
Vizrt's [Software Distribution](https://docs.ndi.video/all/developing-with-ndi/sdk/software-distribution)
page says the same thing in prose: header files may be distributed with
open-source projects under MIT terms and used with dynamic loading of the NDI
libraries — which is exactly what this project does.

That grant covers **the headers only**. The SDK as a whole, and the runtime
library, remain under the NDI SDK License Agreement, which is why neither is in
this repository. See [`docs/06-ndi-distribution.md`](../../docs/06-ndi-distribution.md)
for what may and may not be shipped, and where an installer differs from a
source tree.

## Updating them

Copy the headers from a newer SDK's `include/` directory verbatim. Do not edit
them — the MIT grant is per-file and the notice must travel with each one. Check
that the new set still carries the "applies to this file ONLY" notice before
committing:

```bash
grep -L "MIT license applies to this file ONLY" third_party/ndi/include/*.h
```

Any file listed by that command must not be committed.

**NDI® is a registered trademark of Vizrt NDI AB.** <https://ndi.video>
