// The macOS menu-bar item: an NSStatusItem, a small menu, and nothing else.
//
// Three things here are not obvious and match the rest of this project rather
// than the Objective-C most examples show:
//
// 1. There is no ARC. CMakeLists deliberately avoids enable_language(OBJCXX)
//    (see the note at its head) and nothing adds -fobjc-arc, so this file is
//    manual retain/release exactly as src/outputs/screen_window_mac.mm is.
//    The controller and the status item are owned by this file and released in
//    removeTray().
//
// 2. Deployment target is 12.0 (Info.plist.in), so SF Symbols
//    (+imageWithSystemSymbolName:, macOS 11) are available — but a symbol name
//    that a future macOS renames would return nil, so the image falls back to a
//    short text title rather than leaving an invisible zero-width item in the
//    menu bar that an operator cannot find.
//
// 3. The menu is rebuilt-in-place on open, not on a timer. `menuNeedsUpdate:`
//    fires only when someone actually pulls the menu down, which is the only
//    moment the live line has to be true.
//
// Why a status item is allowed here at all when docs/04-verification.md
// section 9 removed the operator window: that window was a CEF *browser*, and
// the crash was a runtime-style conflict between a windowed browser and the
// windowless sources. AppKit owns no browser. See the comment on installTray()
// in app/tray.h.

#import <Cocoa/Cocoa.h>

#include <string>

#include "app/mac_application.h"
#include "app/tray.h"
#include "diag/diag.h"

namespace {

/// Copied rather than referenced: the caller's TrayOptions is a local in main()
/// and the menu outlives the call that installed it.
weblinked::TrayOptions g_options;

NSString* toNSString(const std::string& text) {
  NSString* value = [NSString stringWithUTF8String:text.c_str()];
  return value != nil ? value : @"";
}

}  // namespace

/// Target for the menu items and delegate for the menu. AppKit needs an
/// Objective-C object for both; everything it forwards to is the C++ in
/// g_options.
@interface WebLinkedTrayController : NSObject <NSMenuDelegate> {
 @private
  NSStatusItem* item_;
  NSMenuItem* statusLine_;
}
- (BOOL)install;
- (void)remove;
@end

@implementation WebLinkedTrayController

- (BOOL)install {
  // NSSquareStatusItemLength rather than variable: the content is an icon, and
  // a variable-length item with a nil image collapses to nothing.
  item_ = [[[NSStatusBar systemStatusBar]
      statusItemWithLength:NSSquareStatusItemLength] retain];
  if (item_ == nil || item_.button == nil) {
    // No window server session. Not an error — see installTray().
    [item_ release];
    item_ = nil;
    return NO;
  }

  NSImage* icon = [NSImage imageWithSystemSymbolName:@"dot.radiowaves.left.and.right"
                            accessibilityDescription:@"WebLinked"];
  if (icon != nil) {
    // A template image is recoloured by AppKit for a light or dark menu bar,
    // and for the highlighted state while the menu is open. Without this the
    // icon is invisible against one of the two.
    //
    // -setTemplate: rather than the `icon.template` property syntax: this is
    // Objective-C++ and `template` is a C++ keyword, so the dot form does not
    // parse at all.
    [icon setTemplate:YES];
    item_.button.image = icon;
  } else {
    item_.button.title = @"WL";
  }
  item_.button.toolTip = toNSString(g_options.appName + " " + g_options.version);

  NSMenu* menu = [[NSMenu alloc] initWithTitle:toNSString(g_options.appName)];
  menu.delegate = self;
  // Otherwise AppKit greys out every item whose target does not answer
  // -validateMenuItem:, which for a menu built by hand is all of them.
  menu.autoenablesItems = NO;

  NSMenuItem* header = [[NSMenuItem alloc]
      initWithTitle:toNSString(g_options.appName + " " + g_options.version)
             action:nullptr
      keyEquivalent:@""];
  header.enabled = NO;
  [menu addItem:header];
  [header release];

  statusLine_ = [[NSMenuItem alloc] initWithTitle:@""
                                           action:nullptr
                                    keyEquivalent:@""];
  statusLine_.enabled = NO;
  [menu addItem:statusLine_];

  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem* open = [[NSMenuItem alloc] initWithTitle:@"Open control page"
                                                action:@selector(openControlPage:)
                                         keyEquivalent:@""];
  open.target = self;
  [menu addItem:open];
  [open release];

  NSMenuItem* copy = [[NSMenuItem alloc] initWithTitle:@"Copy control address"
                                                action:@selector(copyAddress:)
                                         keyEquivalent:@""];
  copy.target = self;
  [menu addItem:copy];
  [copy release];

  NSMenuItem* logs = [[NSMenuItem alloc] initWithTitle:@"Reveal log in Finder"
                                                action:@selector(revealLog:)
                                         keyEquivalent:@""];
  logs.target = self;
  [menu addItem:logs];
  [logs release];

  [menu addItem:[NSMenuItem separatorItem]];

  // ⌘Q on the item itself. The application menu is still empty — this process
  // never sets NSApp.mainMenu — so this is the only way to quit from the UI,
  // which is one of the two complaints section 9 recorded about the window that
  // was removed.
  NSMenuItem* quit = [[NSMenuItem alloc]
      initWithTitle:toNSString("Quit " + g_options.appName)
             action:@selector(quit:)
      keyEquivalent:@"q"];
  quit.target = self;
  [menu addItem:quit];
  [quit release];

  item_.menu = menu;
  [menu release];  // the status item owns it now

  [self refresh];
  return YES;
}

- (void)remove {
  if (item_ != nil) {
    [[NSStatusBar systemStatusBar] removeStatusItem:item_];
    [item_ release];
    item_ = nil;
  }
  // Owned by the menu, which the status item owned; the release above took the
  // whole tree with it.
  statusLine_ = nil;
}

- (void)refresh {
  if (!g_options.status) {
    statusLine_.hidden = YES;
    return;
  }
  statusLine_.title = toNSString(g_options.status());
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  [self refresh];
}

- (void)openControlPage:(id)sender {
  weblinked::openInDefaultBrowser(g_options.controlUrl);
}

- (void)copyAddress:(id)sender {
  NSPasteboard* board = [NSPasteboard generalPasteboard];
  [board clearContents];
  [board setString:toNSString(g_options.controlUrl)
           forType:NSPasteboardTypeString];
}

- (void)revealLog:(id)sender {
  NSString* path = toNSString(weblinked::diag::logFilePath());
  if (path.length == 0) {
    return;
  }
  // selectFile: rather than openFile:, so Finder highlights the log in its
  // folder instead of handing a live, growing file to a text editor.
  [[NSWorkspace sharedWorkspace] selectFile:path
                   inFileViewerRootedAtPath:@""];
}

- (void)quit:(id)sender {
  if (g_options.quit) {
    weblinked::diag::info("shutdown requested from the menu bar");
    g_options.quit();
  }
}

@end

namespace {
WebLinkedTrayController* g_controller = nil;
}

namespace weblinked {

bool installTray(const TrayOptions& options) {
  @autoreleasepool {
    if (g_controller != nil) {
      return true;
    }
    g_options = options;

    WebLinkedTrayController* controller = [[WebLinkedTrayController alloc] init];
    if (![controller install]) {
      [controller release];
      diag::info("no menu bar available — running without a tray icon");
      return false;
    }
    g_controller = controller;

    // Only once there is somewhere else to quit from. Accessory drops the Dock
    // icon and the ⌘-Tab entry, which for a process with no window is right:
    // clicking that icon could never do anything, and section 9 recorded "no
    // way to quit it from the UI" as a real fault. Done at run time rather than
    // by putting LSUIElement in Info.plist so that --no-tray, and any launch
    // where no menu bar exists, keeps the old behaviour exactly.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    diag::info("menu bar item installed");
    return true;
  }
}

void removeTray() {
  @autoreleasepool {
    if (g_controller == nil) {
      return;
    }
    [g_controller remove];
    [g_controller release];
    g_controller = nil;
  }
}

}  // namespace weblinked
