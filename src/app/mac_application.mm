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
// Kept even though this process no longer opens any window at all. Providing
// the subclass is the documented requirement for *every* CEF macOS application,
// not only windowed ones — CefInitialize instantiates NSApp regardless, and
// anything that later pumps a real event through it would hit the selector
// above. It costs one class and removes a whole category of crash, so it stays.
//
// It was originally added for the operator window, which is gone; see
// docs/04-verification.md section 9.

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
