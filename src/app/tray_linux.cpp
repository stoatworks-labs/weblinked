// The Linux menu-bar item: a StatusNotifierItem, by way of libayatana-appindicator.
//
// Four decisions here are deliberate and worth not re-litigating.
//
// 1. **libayatana rather than hand-written D-Bus.** StatusNotifierItem is not
//    one protocol but two — org.kde.StatusNotifierItem for the icon and
//    com.canonical.dbusmenu for the menu — and the second is a tree with
//    revisions, layout diffs and per-item property groups. Implementing both by
//    hand is several hundred lines that exist only to be subtly wrong on one
//    desktop. libayatana is the implementation every GTK application on this
//    desktop already uses.
//
// 2. **Opened with Dylib, not linked.** Exactly why libndi, libomt and avahi
//    are: a machine without the library loses the tray and keeps everything
//    else. WebLinked's whole point is running headless on a box that may have
//    no desktop at all, and a hard link would refuse to start there.
//
// 3. **Headers are used even though the symbols are dlopen'd.** Same split as
//    avahi: the compiler checks every signature against the real declarations,
//    while the loader decides at run time whether the thing exists. Hand-typed
//    prototypes for a C API taking varargs-ish GObject arguments is where this
//    would otherwise go wrong. If the headers are absent at build time the
//    whole file compiles out and installTray() returns false.
//
// 4. **Created on the CEF UI thread, and nothing else touches it.** Chromium's
//    Linux UI thread runs a GLib message pump, so GTK signal handlers dispatch
//    from CefRunMessageLoop() without a gtk_main() of our own. That is the
//    thing to check first if a menu item ever stops responding — see
//    docs/04-verification.md.
//
// This is not a browser window and does not bring back the failure that removed
// the operator window; see the comment on installTray() in app/tray.h.

#include "app/tray.h"

#include <string>

// Despite the name, this header declares openInDefaultBrowser for every
// platform; main.cpp carries the non-Apple definition.
#include "app/mac_application.h"
#include "diag/diag.h"

#if defined(WEBLINKED_WITH_APPINDICATOR)

#include <libayatana-appindicator/app-indicator.h>

#include "core/dylib.h"

namespace weblinked {
namespace {

/// The subset of three libraries this needs. Grouped by the library that
/// provides them, because they are opened separately and a missing GTK is a
/// different diagnosis from a missing appindicator.
struct TrayApi {
  // libayatana-appindicator
  AppIndicator* (*indicator_new)(const gchar*, const gchar*,
                                 AppIndicatorCategory) = nullptr;
  void (*indicator_set_status)(AppIndicator*, AppIndicatorStatus) = nullptr;
  void (*indicator_set_menu)(AppIndicator*, GtkMenu*) = nullptr;
  void (*indicator_set_title)(AppIndicator*, const gchar*) = nullptr;

  // libgtk-3
  gboolean (*gtk_init_check)(int*, char***) = nullptr;
  GtkWidget* (*menu_new)() = nullptr;
  GtkWidget* (*menu_item_new_with_label)(const gchar*) = nullptr;
  GtkWidget* (*separator_menu_item_new)() = nullptr;
  void (*menu_shell_append)(GtkMenuShell*, GtkWidget*) = nullptr;
  void (*widget_show)(GtkWidget*) = nullptr;
  void (*widget_set_sensitive)(GtkWidget*, gboolean) = nullptr;
  void (*menu_item_set_label)(GtkMenuItem*, const gchar*) = nullptr;

  // libgobject-2.0
  gulong (*signal_connect_data)(gpointer, const gchar*, GCallback, gpointer,
                                GClosureNotify, GConnectFlags) = nullptr;
};

// GTK_MENU_SHELL() and friends are not free casts: they expand to
// g_type_check_instance_cast(), a real call into libgobject, which would make
// this file link against a desktop library and defeat the whole dlopen
// arrangement — the linker says so, with "DSO missing from command line".
//
// These are plain casts instead. That is sound here and only here: every widget
// cast below was created a few lines earlier by this same file, so its runtime
// type is known rather than assumed, and the checked cast could only ever
// confirm what construction already guaranteed. Do not reach for these on a
// widget that arrived from somewhere else.
template <typename To, typename From>
To* gcast(From* value) {
  return reinterpret_cast<To*>(value);
}

TrayApi g_api;
Dylib g_appindicator;
Dylib g_gtk;
Dylib g_gobject;

TrayOptions g_options;
AppIndicator* g_indicator = nullptr;
GtkWidget* g_statusItem = nullptr;
bool g_installed = false;

void onOpenControlPage(GtkMenuItem*, gpointer) {
  openInDefaultBrowser(g_options.controlUrl);
}

void onQuit(GtkMenuItem*, gpointer) {
  if (g_options.quit) {
    diag::info("shutdown requested from the tray");
    g_options.quit();
  }
}

/// Refreshes the live line as the menu opens, rather than on a timer — a menu
/// nobody has opened costs nothing to keep true.
void onMenuShow(GtkWidget*, gpointer) {
  if (g_statusItem != nullptr && g_options.status) {
    g_api.menu_item_set_label(gcast<GtkMenuItem>(g_statusItem),
                              g_options.status().c_str());
  }
}

GtkWidget* addItem(GtkWidget* menu, const std::string& label, GCallback handler) {
  GtkWidget* item = g_api.menu_item_new_with_label(label.c_str());
  if (handler != nullptr) {
    g_api.signal_connect_data(item, "activate", handler, nullptr, nullptr,
                              static_cast<GConnectFlags>(0));
  } else {
    g_api.widget_set_sensitive(item, FALSE);
  }
  g_api.widget_show(item);
  g_api.menu_shell_append(gcast<GtkMenuShell>(menu), item);
  return item;
}

bool resolveSymbols() {
  // The SONAME, not the bare .so: the -dev symlink is what a build machine has
  // and a user's machine has only the runtime package.
  const bool haveIndicator = g_appindicator.open({
      "libayatana-appindicator3.so.1",
      "libayatana-appindicator3.so",
      // Some distributions still ship the pre-fork name, ABI-compatible for
      // everything used here.
      "libappindicator3.so.1",
  });
  if (!haveIndicator) {
    diag::info("no tray: libayatana-appindicator not found (%s)",
               g_appindicator.lastError().c_str());
    return false;
  }
  if (!g_gtk.open({"libgtk-3.so.0", "libgtk-3.so"}) ||
      !g_gobject.open({"libgobject-2.0.so.0", "libgobject-2.0.so"})) {
    diag::info("no tray: GTK 3 not loadable alongside the indicator");
    return false;
  }

  const bool resolved =
      g_appindicator.symbol("app_indicator_new", g_api.indicator_new) &&
      g_appindicator.symbol("app_indicator_set_status", g_api.indicator_set_status) &&
      g_appindicator.symbol("app_indicator_set_menu", g_api.indicator_set_menu) &&
      g_gtk.symbol("gtk_init_check", g_api.gtk_init_check) &&
      g_gtk.symbol("gtk_menu_new", g_api.menu_new) &&
      g_gtk.symbol("gtk_menu_item_new_with_label", g_api.menu_item_new_with_label) &&
      g_gtk.symbol("gtk_separator_menu_item_new", g_api.separator_menu_item_new) &&
      g_gtk.symbol("gtk_menu_shell_append", g_api.menu_shell_append) &&
      g_gtk.symbol("gtk_widget_show", g_api.widget_show) &&
      g_gtk.symbol("gtk_widget_set_sensitive", g_api.widget_set_sensitive) &&
      g_gtk.symbol("gtk_menu_item_set_label", g_api.menu_item_set_label) &&
      g_gobject.symbol("g_signal_connect_data", g_api.signal_connect_data);
  // Absent on older builds; it only improves what a desktop shows on hover.
  g_appindicator.symbol("app_indicator_set_title", g_api.indicator_set_title);

  if (!resolved) {
    diag::info("no tray: the indicator or GTK is missing entry points");
    return false;
  }
  return true;
}

}  // namespace

bool installTray(const TrayOptions& options) {
  if (g_installed) {
    return true;
  }
  g_options = options;

  if (!resolveSymbols()) {
    return false;
  }

  // gtk_init_check rather than gtk_init: no display is an ordinary way to run
  // this program, and gtk_init would abort the process over it.
  if (g_api.gtk_init_check(nullptr, nullptr) == FALSE) {
    diag::info("no tray: no display available for GTK");
    return false;
  }

  GtkWidget* menu = g_api.menu_new();
  addItem(menu, g_options.appName + " " + g_options.version, nullptr);
  g_statusItem = addItem(menu, "", nullptr);

  GtkWidget* separator = g_api.separator_menu_item_new();
  g_api.widget_show(separator);
  g_api.menu_shell_append(gcast<GtkMenuShell>(menu), separator);

  addItem(menu, "Open control page", G_CALLBACK(onOpenControlPage));

  GtkWidget* tail = g_api.separator_menu_item_new();
  g_api.widget_show(tail);
  g_api.menu_shell_append(gcast<GtkMenuShell>(menu), tail);

  addItem(menu, "Quit " + g_options.appName, G_CALLBACK(onQuit));

  g_api.signal_connect_data(menu, "show", G_CALLBACK(onMenuShow), nullptr,
                            nullptr, static_cast<GConnectFlags>(0));
  onMenuShow(nullptr, nullptr);

  g_indicator = g_api.indicator_new("weblinked", "video-display",
                                    APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  if (g_indicator == nullptr) {
    diag::info("no tray: the indicator could not be created");
    return false;
  }
  if (g_api.indicator_set_title != nullptr) {
    g_api.indicator_set_title(g_indicator, g_options.appName.c_str());
  }
  g_api.indicator_set_menu(g_indicator, gcast<GtkMenu>(menu));
  // Last, not first: an indicator goes on the panel the moment it is Active, so
  // setting the status before the menu exists can show an item that does
  // nothing when clicked.
  g_api.indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);

  g_installed = true;
  diag::info("tray item installed via %s", g_appindicator.loadedPath().c_str());
  return true;
}

void removeTray() {
  if (!g_installed) {
    return;
  }
  // Passive is what withdraws the item from the panel. The AppIndicator itself
  // is left for process teardown: unreferencing it while the panel still holds
  // its D-Bus name races the watcher, and this runs seconds before exit.
  g_api.indicator_set_status(g_indicator, APP_INDICATOR_STATUS_PASSIVE);
  g_indicator = nullptr;
  g_statusItem = nullptr;
  g_installed = false;
}

}  // namespace weblinked

#else  // !WEBLINKED_WITH_APPINDICATOR

namespace weblinked {

/// Built without the ayatana-appindicator headers, so there is nothing to load.
/// Deliberately not a hand-written D-Bus fallback: see the head of this file.
bool installTray(const TrayOptions&) {
  diag::info("no tray: built without ayatana-appindicator headers");
  return false;
}

void removeTray() {}

}  // namespace weblinked

#endif
