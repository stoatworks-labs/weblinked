// The Windows notification-area icon: Shell_NotifyIcon, a hidden window, and a
// popup menu.
//
// Four things here are load-bearing and none of them are obvious:
//
// 1. **The window is a normal hidden window, not HWND_MESSAGE.** A
//    message-only window is the tidier choice and is wrong here: it does not
//    receive broadcast messages, and "TaskbarCreated" — which Explorer sends to
//    every top-level window when it restarts — is a broadcast. Miss it and the
//    icon vanishes for good the first time Explorer crashes or is restarted,
//    with the process still running and still on air.
//
// 2. **SetForegroundWindow before TrackPopupMenu, and a WM_NULL after.** Without
//    the first, the menu appears and then ignores clicks; without the second it
//    does not dismiss when the user clicks elsewhere. This is a documented
//    Win32 quirk (KB135788), it looks like a bug in the application, and it has
//    been rediscovered by roughly everyone who has ever written one of these.
//
// 3. **Created on the CEF UI thread.** CefRunMessageLoop() pumps a standard
//    Win32 message loop, so this window's WndProc is dispatched by it and needs
//    no loop of its own — the same arrangement as the macOS and Linux trays.
//
// 4. **Quit sets the shutdown flag rather than quitting the loop.** Identical
//    reasoning to the other two platforms: the menu is being tracked when the
//    click arrives, and unwinding the message loop from inside menu tracking is
//    the one place that is not safe.
//
// This is not a browser window and does not reintroduce the crash that removed
// the operator window; see the comment on installTray() in app/tray.h.

#include "app/tray.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "app/mac_application.h"  // declares openInDefaultBrowser everywhere
#include "diag/diag.h"

namespace weblinked {
namespace {

constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kIconId = 1;

enum MenuCommand : UINT {
  kCmdOpenControlPage = 100,
  kCmdCopyAddress,
  kCmdRevealLog,
  kCmdQuit,
};

TrayOptions g_options;
HWND g_window = nullptr;
NOTIFYICONDATAW g_icon{};
UINT g_taskbarCreated = 0;
bool g_installed = false;

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), size);
  return out;
}

/// Copies into the fixed-size arrays NOTIFYICONDATAW uses, truncating rather
/// than overrunning. szTip is 128 wide characters and a long source name plus a
/// version will reach it.
void copyInto(wchar_t* destination, size_t capacity, const std::wstring& text) {
  const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
  ::wmemcpy(destination, text.c_str(), count);
  destination[count] = L'\0';
}

bool addIcon() {
  g_icon = {};
  g_icon.cbSize = sizeof(g_icon);
  g_icon.hWnd = g_window;
  g_icon.uID = kIconId;
  g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_icon.uCallbackMessage = kCallbackMessage;
  // The application's own icon where it has one — LoadIconW with the first
  // resource — and the stock application icon otherwise, so a build without an
  // .ico still shows something rather than an invisible gap.
  //
  // MAKEINTRESOURCEW(32512) rather than IDI_APPLICATION, and that is not
  // pedantry: this project does not define UNICODE, so IDI_APPLICATION expands
  // through MAKEINTRESOURCE to the *ANSI* MAKEINTRESOURCEA and yields an LPSTR.
  // Passing it to the explicit -W function is a hard error
  // ("cannot convert argument 2 from 'LPSTR' to 'LPCWSTR'"), and every other
  // resource id in this file has the same trap waiting in it.
  constexpr int kStockApplicationIcon = 32512;  // IDI_APPLICATION
  g_icon.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  if (g_icon.hIcon == nullptr) {
    g_icon.hIcon = ::LoadIconW(nullptr, MAKEINTRESOURCEW(kStockApplicationIcon));
  }
  copyInto(g_icon.szTip, ARRAYSIZE(g_icon.szTip),
           widen(g_options.appName + " " + g_options.version));

  // The return value is the only evidence the icon was accepted, and it is
  // routinely thrown away. NIM_ADD fails for ordinary, recoverable reasons —
  // the shell not being ready yet during logon, or an id already in use — and
  // an unchecked call means the log says the icon is installed while the
  // notification area has nothing in it. A line that claims success it did not
  // verify is worse than no line at all.
  if (::Shell_NotifyIconW(NIM_ADD, &g_icon) == FALSE) {
    diag::warn("tray: Shell_NotifyIcon(NIM_ADD) refused the icon (%lu)",
               ::GetLastError());
    return false;
  }

  // Opt into the modern behaviour, which is what makes the callback report
  // WM_CONTEXTMENU and balloon events properly. Non-fatal: an older shell
  // simply keeps the v0 semantics the WndProc already handles.
  g_icon.uVersion = NOTIFYICON_VERSION_4;
  ::Shell_NotifyIconW(NIM_SETVERSION, &g_icon);
  return true;
}

void showMenu() {
  HMENU menu = ::CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  const std::wstring header = widen(g_options.appName + " " + g_options.version);
  ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, header.c_str());

  if (g_options.status) {
    // Read as the menu opens rather than on a timer, so a menu nobody has
    // opened costs nothing to keep true.
    const std::wstring status = widen(g_options.status());
    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, status.c_str());
  }

  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdOpenControlPage, L"Open control page");
  ::AppendMenuW(menu, MF_STRING, kCmdCopyAddress, L"Copy control address");
  ::AppendMenuW(menu, MF_STRING, kCmdRevealLog, L"Reveal log in Explorer");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const std::wstring quit = widen("Quit " + g_options.appName);
  ::AppendMenuW(menu, MF_STRING, kCmdQuit, quit.c_str());

  POINT cursor{};
  ::GetCursorPos(&cursor);
  // See note 2 at the head of this file: both of these calls are required and
  // neither is cosmetic.
  ::SetForegroundWindow(g_window);
  ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
  ::PostMessageW(g_window, WM_NULL, 0, 0);
  ::DestroyMenu(menu);
}

void copyAddress() {
  const std::wstring text = widen(g_options.controlUrl);
  if (!::OpenClipboard(g_window)) {
    return;
  }
  ::EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (handle != nullptr) {
    void* target = ::GlobalLock(handle);
    if (target != nullptr) {
      ::memcpy(target, text.c_str(), bytes);
      ::GlobalUnlock(handle);
      // Ownership passes to the clipboard on success only; freeing it after a
      // successful SetClipboardData would hand the next paste a dead pointer.
      if (::SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
        ::GlobalFree(handle);
      }
    } else {
      ::GlobalFree(handle);
    }
  }
  ::CloseClipboard();
}

void revealLog() {
  const std::wstring path = widen(diag::logFilePath());
  if (path.empty()) {
    return;
  }
  // /select, highlights the file in its folder rather than opening a live,
  // growing log in whatever is registered for .log.
  const std::wstring argument = L"/select,\"" + path + L"\"";
  ::ShellExecuteW(nullptr, L"open", L"explorer.exe", argument.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kCallbackMessage) {
    // NOTIFYICON_VERSION_4 changes the shape of this message, and getting it
    // wrong is silent: the icon appears, every click is delivered, and the menu
    // simply never opens. Under v4 the event is LOWORD(lParam) and the icon id
    // is HIWORD(lParam) — a right-click arrives as WM_CONTEXTMENU rather than
    // WM_RBUTTONUP, so a v0-style `lparam == WM_RBUTTONUP` test never matches.
    //
    // Both encodings are accepted here so the code does not depend on whether
    // NIM_SETVERSION succeeded, which is not something worth finding out from a
    // dead menu on someone else's machine.
    const UINT event = LOWORD(lparam);
    // Logged because the alternative is guessing. When a tray menu does not
    // appear there are two quite different causes — the click never reaching
    // this window, or the menu failing to display once it does — and they look
    // identical from the outside.
    diag::debug("tray: callback event 0x%04x (lparam 0x%llx)", event,
                static_cast<unsigned long long>(lparam));
    const bool wantsMenu = event == WM_CONTEXTMENU || event == WM_RBUTTONUP ||
                           event == NIN_SELECT || event == WM_LBUTTONUP;
    if (wantsMenu) {
      // Either button opens it. Windows convention is right-click, but a
      // left-click on an icon whose application has no window should not do
      // nothing at all.
      showMenu();
    }
    return 0;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
      case kCmdOpenControlPage:
        openInDefaultBrowser(g_options.controlUrl);
        return 0;
      case kCmdCopyAddress:
        copyAddress();
        return 0;
      case kCmdRevealLog:
        revealLog();
        return 0;
      case kCmdQuit:
        if (g_options.quit) {
          diag::info("shutdown requested from the tray");
          g_options.quit();
        }
        return 0;
      default:
        break;
    }
  }
  // Explorer restarted and threw away every icon in the notification area. See
  // note 1 at the head of this file for why this window can hear about it.
  if (g_taskbarCreated != 0 && message == g_taskbarCreated && g_installed) {
    diag::info("tray: Explorer restarted, re-adding the icon");
    // Result ignored on purpose: if the shell is still coming up there is
    // nothing useful to do but leave the icon absent until the next restart.
    (void)addIcon();
    return 0;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool installTray(const TrayOptions& options) {
  if (g_installed) {
    return true;
  }
  g_options = options;

  WNDCLASSEXW klass{};
  klass.cbSize = sizeof(klass);
  klass.lpfnWndProc = wndProc;
  klass.hInstance = ::GetModuleHandleW(nullptr);
  klass.lpszClassName = L"WebLinkedTrayWindow";
  // A second instance in the same process would fail to register the class;
  // that is not an error worth refusing over, so only a genuine failure counts.
  if (::RegisterClassExW(&klass) == 0 &&
      ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    diag::info("no tray: could not register the window class (%lu)",
               ::GetLastError());
    return false;
  }

  // Never shown: WS_OVERLAPPED without WS_VISIBLE, and no ShowWindow call. It
  // exists to own the icon and receive its messages, and deliberately is not a
  // message-only window — see note 1.
  g_window = ::CreateWindowExW(0, klass.lpszClassName, L"WebLinked", WS_OVERLAPPED,
                               0, 0, 0, 0, nullptr, nullptr, klass.hInstance,
                               nullptr);
  if (g_window == nullptr) {
    diag::info("no tray: could not create the tray window (%lu)", ::GetLastError());
    return false;
  }

  g_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
  if (!addIcon()) {
    ::DestroyWindow(g_window);
    g_window = nullptr;
    return false;
  }
  g_installed = true;
  diag::info("tray icon installed");
  return true;
}

void removeTray() {
  if (!g_installed) {
    return;
  }
  ::Shell_NotifyIconW(NIM_DELETE, &g_icon);
  if (g_window != nullptr) {
    ::DestroyWindow(g_window);
    g_window = nullptr;
  }
  g_installed = false;
}

}  // namespace weblinked
