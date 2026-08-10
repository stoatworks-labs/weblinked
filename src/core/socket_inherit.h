#pragma once

#include <cstdint>

namespace weblinked {

/// Stop a socket being handed to child processes.
///
/// This exists because the `stream` output spawns ffmpeg — the first child
/// process this application ever had. A child inherits every open descriptor by
/// default, so ffmpeg came up holding the control port, and a WebLinked that
/// exited while its encoder was still running left the port bound by the
/// orphan. The next launch then failed with "port 7654 is already in use —
/// another WebLinked is probably running", naming the wrong cause.
///
/// Call this on every socket that is created and kept, including the ones
/// accept() returns. It is a no-op on a platform where the flag is already set.
void preventSocketInheritance(std::intptr_t socket);

}  // namespace weblinked
