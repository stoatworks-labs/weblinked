#include "core/socket_inherit.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace weblinked {

void preventSocketInheritance(std::intptr_t socket) {
  if (socket < 0) {
    return;
  }
#if defined(_WIN32)
  // Winsock handles are inheritable by default, and CreateProcess is called
  // with bInheritHandles = TRUE so the child can be given a stderr pipe.
  SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
#else
  const int flags = ::fcntl(static_cast<int>(socket), F_GETFD, 0);
  if (flags >= 0) {
    ::fcntl(static_cast<int>(socket), F_SETFD, flags | FD_CLOEXEC);
  }
#endif
}

}  // namespace weblinked
