// The NSApplication subclass CEF requires on macOS.
//
// Chromium's message pump asks the running NSApplication whether it is currently
// inside -sendEvent:, via the CrAppProtocol selector -isHandlingSendEvent.
// Stock NSApplication does not implement it, so the moment a real window starts
// pumping events the process dies with:
//
//   *** Terminating app due to uncaught exception 'NSInvalidArgumentException',
//   reason: '-[NSApplication isHandlingSendEvent]: unrecognized selector sent
//   to instance ...'
//
// This is easy to miss: an offscreen-only, --headless run never drives NSApp
// this way and works perfectly, so the crash only appears once somebody opens
// the operator window — and then only on the way out, which reads like a
// shutdown bug rather than a missing application class.
//
// Providing the subclass is the documented requirement for every CEF macOS
// application; cefsimple ships the same code.

#import <Cocoa/Cocoa.h>

#include "include/cef_application_mac.h"

#include "app/mac_application.h"

@interface WebLinkedApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation WebLinkedApplication

- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  // The scoper sets and restores the flag above around the call, which is what
  // Chromium is really asking about.
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

@end

namespace weblinked {

void installMacApplication() {
  // +sharedApplication instantiates whichever NSApplication subclass is asked
  // first, so this must run before anything else touches NSApp — in practice,
  // before CefInitialize.
  [WebLinkedApplication sharedApplication];
}

}  // namespace weblinked
