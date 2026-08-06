#!/bin/bash
# Builds unload_probe and the control bundle it is checked against.
#
# The probe links Resolume Arena's Syphon.framework so that a real Syphon is
# loaded in the process — the probe itself looks the classes up by name, so
# what it exercises is whatever the host has, exactly as the plugin does.
#
#   ./build_unload_probe.sh
#   ./out/unload_probe --bundle ./out/BadCategory.bundle --control   # must CRASH
#   ./out/unload_probe --bundle ../build/WebLinked.bundle/Contents/MacOS/WebLinked
#
# The control has to fail. A probe that passes everything proves nothing.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="$here/out"
syphon="/Applications/Resolume Arena/Arena.app/Contents/Frameworks"
mkdir -p "$out"

if [ ! -d "$syphon/Syphon.framework" ]; then
  echo "Resolume Arena's Syphon.framework not found at:" >&2
  echo "  $syphon" >&2
  echo "Any application bundling Syphon will do; adjust the path." >&2
  exit 1
fi

# --- the control: an image that extends a Foundation class ------------------
# This is the shape of the original bug, reduced to nine lines. A category on
# NSArray attaches to a class that outlives the bundle, so unloading leaves its
# method list dangling.
cat > "$out/bad_category.m" <<'EOF'
#import <Foundation/Foundation.h>
@interface NSArray (WebLinkedUnloadProbeControl)
- (NSUInteger)webLinkedUnloadProbeCanary;
@end
@implementation NSArray (WebLinkedUnloadProbeControl)
- (NSUInteger)webLinkedUnloadProbeCanary { return [self count]; }
@end
EOF

clang -bundle -fobjc-arc -o "$out/BadCategory.bundle" "$out/bad_category.m" \
  -framework Foundation

clang++ -std=c++20 -fno-objc-arc -Wno-deprecated-declarations \
  -o "$out/unload_probe" "$here/unload_probe.mm" \
  -F "$syphon" -framework Syphon -framework Foundation -framework OpenGL \
  -rpath "$syphon"

# --- the supervision probe --------------------------------------------------
# Links Helper.cpp itself, so what is exercised is the shipping code rather
# than a restatement of it.
clang++ -std=c++17 -fno-objc-arc -Wno-deprecated-declarations \
  -I"$here/../source" \
  -o "$out/helper_probe" "$here/helper_probe.mm" "$here/../source/Helper.cpp" \
  -F "$syphon" -framework Syphon -framework Foundation \
  -rpath "$syphon"

echo "built:"
echo "  $out/unload_probe"
echo "  $out/BadCategory.bundle   (the control — this one must crash)"
echo "  $out/helper_probe         (needs WEBLINKED_BINARY or an installed WebLinked)"

# --- the render probe -------------------------------------------------------
# Drives the built bundle through plugMain into an offscreen framebuffer and
# checks the pixels. Answers "does it actually draw the page" without a GUI.
# Links Syphon so that one is LOADED in the process. The plugin deliberately
# ships no Syphon of its own and looks the classes up with NSClassFromString,
# so a host without Syphon gives it nothing to attach to — which is correct
# behaviour, and means this probe has to stand in for Resolume by having the
# framework present.
clang++ -std=c++17 -fno-objc-arc -Wno-deprecated-declarations \
  -o "$out/render_probe" "$here/render_probe.mm" \
  -F "$syphon" -framework Syphon -framework Foundation -framework OpenGL \
  -rpath "$syphon"

echo "  $out/render_probe         (needs WEBLINKED_BINARY)"
