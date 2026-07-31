#pragma once

namespace weblinked {

/// Installs the NSApplication subclass CEF requires on macOS.
///
/// Must be called before CefInitialize, and before anything else touches NSApp.
/// Without it the process dies with an "unrecognized selector
/// -[NSApplication isHandlingSendEvent]" exception the moment a real window
/// starts pumping events — see mac_application.mm.
void installMacApplication();

}  // namespace weblinked
