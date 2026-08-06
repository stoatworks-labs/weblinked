# The Resolume plugin

A web page as a source *inside* a Resolume composition, in `plugin/`.

Status: **builds and is safe to load; not yet seen drawing in Arena.** See
`docs/04-verification.md` sections 25 and 26 for exactly what has been
established.

---

## Why the browser is not in the plugin

The obvious reading of "a browser source for Resolume" is a plugin containing
Chromium. That cannot be built, and the reasons are structural rather than a
matter of effort:

* **`CefInitialize` wants the main thread's run loop.** WebLinked calls it and
  then `CefRunMessageLoop()` on the main thread. In a plugin, the main thread
  and its run loop belong to Resolume.
* **On macOS, CEF needs `NSApp` to be a `CrAppProtocol` subclass, installed
  before `CefInitialize`.** Arena has already built its own `NSApplication`
  long before a plugin bundle is loaded.
* **Chromium replaces the process's signal and crash handlers.** In-process,
  the plugin would silently take over Arena's crash reporting.
* **CEF dispatches its own subprocesses through `argv` in `main()`.** A plugin
  has no `main()`.
* **One CEF per process.** Two instances of the plugin would mean a second
  `CefInitialize`, which fails.

So "natively inside Arena" means the *plugin* is native, not the browser. The
page is rendered by a WebLinked process and arrives over Syphon.

That is not a consolation prize. It is also why a page that hangs, or a WebGL
context that takes Chromium's GPU process down, costs a black layer instead of
the show.

## Why it exists at all, given Resolume has Syphon input

Resolume can already show a WebLinked feed through its own Syphon source. A
plugin that only attached to one would be worth nothing.

What it adds is that **the URL lives in the composition**: saved with the comp,
set per clip, deck-triggerable, and with no second application for an operator
to start, find again and keep alive. The plugin owns a WebLinked process per
instance and shuts it down with the layer.

## The two modes

| Mode | What happens |
|---|---|
| **Run WebLinked** (default) | The plugin launches a WebLinked for this instance, renders `URL` in it, and shows the result. `Source Name` is only a stem — a per-instance suffix is added so two layers cannot publish under one name. |
| **Attach** | Shows an existing Syphon source called `Source Name`, started by somebody else. What Resolume's own input does, kept as the honest fallback when a WebLinked is already running. |

`Run` off, or an empty `URL`, means nothing is launched and nothing is drawn.

## The rules that bite

**1. An empty URL must stay inert.** Resolume instantiates every plugin it
finds when scanning a folder. A fresh instance has an empty `URL`, and that is
the only thing standing between a plugin scan and a browser launched per
installed plugin. The guard is in `ProcessOpenGL`, and it is load-bearing.

**2. This bundle must contribute no Objective-C metadata.** Not a class, not a
category, not a protocol. Resolume `dlclose`s a plugin after inspecting it, and
an Objective-C image that has extended somebody else's class cannot be safely
unloaded — the first version of this plugin crashed Arena during a folder scan
for exactly that reason. Renaming classes does not help; a category attaches to
its target whatever the contributing image is called. The plugin borrows the
host's Syphon through `NSClassFromString` instead. Check with:

```bash
otool -l plugin/build/WebLinked.bundle/Contents/MacOS/WebLinked \
  | grep -E "__objc_classlist|__objc_catlist|__objc_protolist"
```

Anything printed is a bug. `plugin/tools/unload_probe` checks it for you, and
carries a control that must keep crashing.

**3. Every text parameter must accept a set.** `instantiateGL` sets every
parameter's default on a fresh instance and destroys the instance if any set
returns `FF_FAIL` — and the SDK's base `SetTextParameter` is a stub that
returns exactly that. A plugin declaring a text parameter without overriding it
cannot be created by any host, while a harness driving the C++ class directly
passes happily. Only a probe going through `plugMain` sees it.

**4. `weblinked_plugin_core` is an OBJECT library, not STATIC.**
`CFFGLPluginInfo` registers itself from a file-scope constructor and nothing
references it by name, so in an archive the linker may drop the translation
unit — giving a bundle that loads, exports `plugMain`, and contains no plugins.

**5. Syphon hands out a `GL_TEXTURE_RECTANGLE`.** Unnormalised coordinates, so
the shader takes `sampler2DRect` and multiplies by the texture size. A
`sampler2D` here samples one texel.

**6. The host must already have Syphon loaded.** A direct consequence of rule
2: the plugin ships no Syphon, so `NSClassFromString(@"SyphonServerDirectory")`
returns nil in a host that has none, and the plugin draws nothing. That is
correct behaviour rather than a bug — Resolume always bundles Syphon — but it
means any harness standing in for a host has to link the framework to be
meaningful. `render_probe` does; the first version did not, and reported a
working plugin as drawing nothing.

## Build and install

```bash
cd plugin
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

cp -R build/WebLinked.bundle "$HOME/Documents/Resolume Arena/Extra Effects/"
```

Separate from the top-level build on purpose, exactly as `launcher/` is: it
needs the FFGL SDK, produces a Resolume bundle rather than an application, and
contains no CEF.

## Check before installing

Never put a build of this in a plugin folder without running the probes. The
first version took Arena down at scan time, before anything was placed on a
layer, and a plugin folder is not a good place to discover that.

```bash
cd plugin/tools
./build_unload_probe.sh

./out/unload_probe --bundle ./out/BadCategory.bundle --control   # must CRASH (139)
./out/unload_probe --bundle ../build/WebLinked.bundle/Contents/MacOS/WebLinked

WEBLINKED_BINARY=/path/to/WebLinked ./out/helper_probe
WEBLINKED_BINARY=/path/to/WebLinked ./out/render_probe   # does it draw the page?
```

The control has to keep failing. A probe that passes everything proves nothing.

## Finding WebLinked

`Helper::findBinary()` looks at `$WEBLINKED_BINARY` first — the only way to
test against a build tree — then `/Applications/WebLinked.app` and
`~/Applications/WebLinked.app`.
