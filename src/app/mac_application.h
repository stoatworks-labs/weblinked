#pragma once

#include <string>

namespace weblinked {

/// Installs the NSApplication subclass CEF requires on macOS.
///
/// Must be called before CefInitialize, and before anything else touches NSApp.
/// Without it the process dies with an "unrecognized selector
/// -[NSApplication isHandlingSendEvent]" exception the moment a real window
/// starts pumping events — see mac_application.mm.
void installMacApplication();

/// Hands a URL to the user's default browser.
///
/// Deliberately *not* a browser of our own. Every browser in this process is
/// windowless, which forces Alloy runtime style, and a windowed one would
/// default to Chrome style and segfault the GPU process — the empty-window bug
/// that shipped up to v0.5.2. Another application's window has no such problem.
void openInDefaultBrowser(const std::string& url);

}  // namespace weblinked
