// Syphon server, macOS.
//
// Syphon shares a frame between applications as an IOSurface — a buffer both
// the CPU and the GPU can address, and which a mach port can hand to another
// process without a copy. The framework's own servers exist to get a GL or
// Metal texture *into* such a surface. We already have the pixels on the CPU,
// because that is what CEF's OnPaint gives us, so this backend skips both
// renderers and writes the surface directly:
//
//     newSurfaceForWidth:height:options:   (SyphonSubclassing, BGRA8)
//     IOSurfaceLock -> copy rows -> IOSurfaceUnlock
//     publish                              (SyphonSubclassing)
//
// That is the whole path. No GL context, no Metal device, no shaders, and
// therefore none of the vendored framework's renderers — see
// third_party/syphon/README.md for what was taken and what was left.
//
// Orientation is a straight copy: `SyphonMetalServer` blits without inverting
// when told its source is unflipped, so the surface's first row is the top of
// the image, which is exactly what CEF paints. Do not add a flip here.
//
// Verified against Resolume Arena's bundled Syphon 5 client — the protocol
// (announce notifications, description keys, SyphonSurfaceTypeIOSurface) is
// unchanged between 5 and 6. See docs/04-verification.md.

#include "outputs/shared_surface.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonServerBase.h>
#import <Syphon/SyphonSubclassing.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "diag/diag.h"

/// Exposes the two SyphonSubclassing hooks this backend needs, and nothing
/// else. Declared as a subclass rather than calling the category directly on a
/// SyphonServerBase so that "which parts of Syphon we actually use" is one
/// short interface instead of a grep.
@interface WebLinkedSyphonServer : SyphonServerBase
/// A BGRA8 IOSurface of this size. Retained; the caller CFReleases it.
/// Syphon caches one internally and hands back the same surface while the
/// dimensions are unchanged.
- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height;
/// Announces that the surface holds a new frame.
- (void)publishFrame;
@end

@implementation WebLinkedSyphonServer

- (IOSurfaceRef)newSurfaceForWidth:(size_t)width height:(size_t)height {
  return [self newSurfaceForWidth:width height:height options:nil];
}

- (void)publishFrame {
  [self publish];
}

@end

namespace weblinked {
namespace {

/// AppKit's rule, and here Syphon's too. See open() for why this matters more
/// than it looks. The isMainThread test is not an optimisation — a
/// dispatch_sync to the main queue *from* the main thread deadlocks outright.
void runOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

class SyphonSurface final : public SharedSurface {
 public:
  ~SyphonSurface() override { close(); }

  bool open(const VideoFormat& format, const std::string& name,
            std::string& error) override {
    close();

    __block WebLinkedSyphonServer* server = nil;
    __block IOSurfaceRef surface = nullptr;
    NSString* serverName = [NSString stringWithUTF8String:name.c_str()];
    const size_t width = static_cast<size_t>(format.width);
    const size_t height = static_cast<size_t>(format.height);

    // On the main thread, and not merely by convention. SyphonServerBase
    // registers for the announce-request notification from -init, on whichever
    // thread called it, and NSDistributedNotificationCenter delivers to that
    // thread's run loop. Created on the HTTP thread — which is what happens
    // when an operator adds this output from the control page — the server
    // would post its opening announce and then never answer another discovery
    // request. A consumer already running would find it; one started later
    // never would. That is a fault that survives every short test and appears
    // on the night, so it is prevented here rather than documented.
    // CefRunMessageLoop pumps the main run loop, so the main thread is the one
    // thread guaranteed to service them.
    runOnMain(^{
      server = [[WebLinkedSyphonServer alloc] initWithName:serverName options:nil];
      if (server != nil) {
        surface = [server newSurfaceForWidth:width height:height];
      }
    });

    if (server == nil) {
      error = "Syphon server '" + name + "' could not be started";
      return false;
    }
    if (surface == nullptr) {
      runOnMain(^{
        [server stop];
      });
      error = "Syphon could not allocate a " + std::to_string(format.width) + "x" +
              std::to_string(format.height) + " IOSurface";
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    server_ = server;
    surface_ = surface;
    format_ = format;
    published_.store(0, std::memory_order_relaxed);
    skipped_.store(0, std::memory_order_relaxed);
    return true;
  }

  void close() override {
    // Detach under the lock, tear down outside it. close() marshals to the
    // main thread, and holding the lock across that dispatch_sync would let a
    // busy main thread block the clock thread inside publish() — the one thing
    // publish() promises not to do. Once the members are null, publish() is
    // already a no-op and the teardown has nothing racing it.
    WebLinkedSyphonServer* server = nil;
    IOSurfaceRef surface = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::swap(server, server_);
      std::swap(surface, surface_);
    }
    if (surface != nullptr) {
      CFRelease(surface);
    }
    if (server != nil) {
      runOnMain(^{
        [server stop];
      });
    }
  }

  void publish(const VideoFrame& frame) override {
    if (frame.pixelFormat() != PixelFormat::kBGRA) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (server_ == nil || surface_ == nullptr) {
      return;
    }

    // The same guard frameInFormat and the clock loop's stale-frame drop keep:
    // a frame from the old raster must never be copied into a surface sized for
    // the new one. Dropping it costs one frame at a format change.
    if (frame.format().width != format_.width ||
        frame.format().height != format_.height) {
      return;
    }

    // Syphon's own advice, and the reason skippedCount() exists: with nobody
    // attached there is no point copying 8 MB fifty times a second.
    if (![server_ hasClients]) {
      skipped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (IOSurfaceLock(surface_, 0, nullptr) != kIOReturnSuccess) {
      return;
    }
    auto* dst = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface_));
    const size_t dstStride = IOSurfaceGetBytesPerRow(surface_);
    const auto* src = frame.data();
    const size_t srcStride = static_cast<size_t>(frame.rowBytes());
    // IOSurface pads its rows to its own alignment, which is not the frame's,
    // so this is row by row rather than one memcpy. Equal strides are the
    // common case and the copy is still one call per row; measuring a fused
    // path made no difference against the cost of the pixels themselves.
    const size_t rowBytes = std::min(srcStride, dstStride);
    for (int y = 0; y < format_.height; ++y) {
      std::memcpy(dst + (static_cast<size_t>(y) * dstStride),
                  src + (static_cast<size_t>(y) * srcStride), rowBytes);
    }
    IOSurfaceUnlock(surface_, 0, nullptr);

    [server_ publishFrame];
    published_.fetch_add(1, std::memory_order_relaxed);
  }

  bool hasClients() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_ != nil && [server_ hasClients];
  }

  int64_t publishedCount() const override {
    return published_.load(std::memory_order_relaxed);
  }

  int64_t skippedCount() const override {
    return skipped_.load(std::memory_order_relaxed);
  }

  std::string describe() const override {
    return "Syphon 6 server, IOSurface BGRA8, CPU copy";
  }

 private:
  mutable std::mutex mutex_;
  WebLinkedSyphonServer* server_ = nil;
  IOSurfaceRef surface_ = nullptr;
  VideoFormat format_;
  std::atomic<int64_t> published_{0};
  std::atomic<int64_t> skipped_{0};
};

}  // namespace

std::unique_ptr<SharedSurface> createSharedSurface() {
  return std::make_unique<SyphonSurface>();
}

const char* sharedSurfaceProtocol() { return "Syphon"; }

}  // namespace weblinked
