#include "WebLinkedPlugin.h"

/**
    The plugin registration, and only that.

    **This file is listed directly in the plugin target, not in the shared
    object library.** `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC archive
    the linker is entitled to drop the whole translation unit — giving a bundle
    that loads, exports `plugMain`, and reports that it contains no plugins.
    The check:

        nm -gU WebLinked.bundle/Contents/MacOS/WebLinked | grep plugMain
*/
namespace {
class WebLinkedSource : public weblinked::WebLinkedPlugin {};
}  // namespace

static CFFGLPluginInfo PluginInfo(
    PluginFactory<WebLinkedSource>,  // Create method
    "WL01",                          // Plugin unique ID of maximum length 4
    "WebLinked",                     // Plugin name
    2,                               // API major version number
    1,                               // API minor version number
    0,                               // Plugin major version number
    1,                               // Plugin minor version number
    FF_SOURCE,                       // Plugin type
    "A live web page as a source",   // Plugin description
    "WebLinked FFGL source"          // About
);
