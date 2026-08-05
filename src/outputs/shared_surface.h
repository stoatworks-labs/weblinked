#pragma once

#include <memory>
#include <string>

#include "core/frame.h"
#include "core/video_format.h"

namespace weblinked {

/// A frame published into another application's process as a GPU surface —
/// Syphon on macOS, Spout on Windows — without ever leaving the GPU or the
/// machine.
///
/// Two implementations, one per platform, chosen at build time. There is no
/// runtime dispatch because a binary only ever has one of them, exactly as for
/// ScreenWindow.
///
/// **Why this is an output and not something else.** The same argument as the
/// preview and the screen: it travels the frame path SDI and NDI travel, so a
/// Syphon source that looks right is evidence the others are right. It is also
/// the only output here whose consumer is another *application* rather than a
/// device or a network, which changes exactly one thing — the consumer pulls
/// when it pleases, so there is no pacing to honour and no queue to keep.
/// Publish the newest frame and return.
///
/// The threading contract, stated rather than implied:
///
///   - Every method is callable from any thread. `open()` arrives on the main
///     thread at boot but on the HTTP thread when an operator adds an output
///     from the control page. Implementations marshal internally wherever the
///     platform demands it.
///   - `publish()` is called from the engine's clock thread and must not
///     block. It copies the frame into the shared surface and returns; nothing
///     about it waits for a consumer.
///
/// **Orientation.** The surface holds BGRA8 with the *first row at the top* of
/// the image, which is what CEF paints and what Syphon's own servers write when
/// told the source is unflipped (`SyphonMetalServer` blits straight through in
/// that case). So the copy is a copy — resist the urge to add a vertical flip
/// here. If a consumer shows the page upside down the bug is at the consumer's
/// end of the convention, not in this buffer.
///
/// **Alpha stays premultiplied.** Chromium paints premultiplied and every
/// consumer of these protocols expects premultiplied, so nothing is undone on
/// the way out. This is the reason a shared surface beats NDI for an overlay:
/// the alpha the page authored arrives intact on the consumer's layer.
class SharedSurface {
 public:
  virtual ~SharedSurface() = default;

  /// Publishes a surface under `name`, which is what a consumer's source list
  /// will show. Returns false with `error` set to something an operator can
  /// act on.
  ///
  /// The name need not be unique — the protocols disambiguate by process — but
  /// two WebLinked sources publishing the same name is confusing rather than
  /// wrong, and the engine does not police it.
  virtual bool open(const VideoFormat& format, const std::string& name,
                    std::string& error) = 0;

  virtual void close() = 0;

  /// Hands over one frame, which must be kBGRA. Copies; the reference does not
  /// outlive the call.
  virtual void publish(const VideoFrame& frame) = 0;

  /// True when at least one consumer is attached.
  ///
  /// Not merely a statistic: both protocols recommend skipping the copy
  /// entirely when nobody is listening, and an idle Syphon source that still
  /// memcpys 8 MB fifty times a second is the kind of cost that only shows up
  /// as someone else's dropped frames.
  virtual bool hasClients() const = 0;

  /// Frames actually copied into the surface and announced.
  virtual int64_t publishedCount() const = 0;

  /// Frames dropped because no consumer was attached. Expected to be large on
  /// an idle system and is not an error — it is the optimisation working.
  virtual int64_t skippedCount() const = 0;

  /// A short description of the path actually in use, for the control page's
  /// status — "Syphon 6 (IOSurface, BGRA8)" and so on.
  virtual std::string describe() const = 0;
};

/// Constructs the platform's implementation. Never null.
std::unique_ptr<SharedSurface> createSharedSurface();

/// What this platform's protocol is called, for the control page and the log:
/// "Syphon" or "Spout". Safe to call before any surface exists.
const char* sharedSurfaceProtocol();

}  // namespace weblinked
