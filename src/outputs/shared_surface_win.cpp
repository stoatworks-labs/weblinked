// Spout sender, Windows.
//
// Spout shares a frame between applications as a DirectX 11 texture with a
// DXGI shared handle, named through shared memory so a receiver can find it by
// name. The SDK's `spoutDX` class is the DirectX-only half — no OpenGL, no
// window, no message pump — which is what makes this backend the direct
// counterpart of the Syphon one:
//
//     OpenDirectX11(nullptr)     spoutDX creates its own D3D11 device
//     SetSenderName(name)
//     SendImage(pixels, w, h, pitch)
//
// `SendImage` calls `UpdateSubresource` straight onto the shared texture with
// the pitch we hand it, so the frame's own `rowBytes` goes through unmodified.
// And spoutDX's default sender format is DXGI_FORMAT_B8G8R8A8_UNORM, which is
// exactly PixelFormat::kBGRA — the same fortunate match as the IOSurface on
// macOS. There is no conversion, no flip and no intermediate buffer anywhere in
// this file, and there should not be one: see the orientation note in
// shared_surface.h.
//
// **This file has never been run.** Like the Windows screen output beside it,
// it compiles against the Windows SDK and nothing more — see
// docs/04-verification.md, and treat every claim here as intent rather than
// evidence.
//
// One deliberate difference from Syphon, which is a property of the protocol
// rather than a shortcut: Spout gives a *sender* no way to learn whether
// anyone is receiving. `spoutDX::IsConnected` is the receiver's question
// ("connected to a sender"). So `hasClients()` is always true here and the
// skip-when-nobody-is-listening optimisation that Syphon gets does not exist —
// `skipped` stays at zero and every tick pays for the copy. Reporting a
// truthful "we cannot tell" beats inventing an attach signal that would be
// wrong in whichever direction was least convenient.

#include "outputs/shared_surface.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "SpoutDX.h"

namespace weblinked {
namespace {

class SpoutSurface final : public SharedSurface {
 public:
  ~SpoutSurface() override { close(); }

  bool open(const VideoFormat& format, const std::string& name,
            std::string& error) override {
    close();
    std::lock_guard<std::mutex> lock(mutex_);

    // A null device asks spoutDX to create its own. Sharing the screen
    // output's device would couple two outputs that are meant to be
    // independent — and a source may well have a Spout sender and no screen.
    if (!sender_.OpenDirectX11(nullptr)) {
      error = "Spout could not open a DirectX 11 device";
      return false;
    }

    // Named before the first send: SendImage would otherwise create the sender
    // under spoutDX's own default name, and an operator looking for the name
    // they typed would not find it.
    if (!sender_.SetSenderName(name.c_str())) {
      sender_.CloseDirectX11();
      error = "Spout could not create a sender named '" + name + "'";
      return false;
    }

    format_ = format;
    published_.store(0, std::memory_order_relaxed);
    open_ = true;
    return true;
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return;
    }
    open_ = false;
    // Releasing the sender retires the name, so a receiver's source list loses
    // the entry now rather than keeping a dead one until it times out.
    sender_.ReleaseSender();
    sender_.CloseDirectX11();
  }

  void publish(const VideoFrame& frame) override {
    if (frame.pixelFormat() != PixelFormat::kBGRA) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return;
    }

    // The same guard frameInFormat and the clock loop's stale-frame drop keep:
    // a frame from the old raster must never be sent as though it were the new
    // one. Dropping it costs one frame at a format change.
    if (frame.format().width != format_.width ||
        frame.format().height != format_.height) {
      return;
    }

    if (sender_.SendImage(frame.data(), static_cast<unsigned int>(format_.width),
                          static_cast<unsigned int>(format_.height),
                          static_cast<unsigned int>(frame.rowBytes()))) {
      published_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  /// Always true — Spout tells a sender nothing about receivers. See the note
  /// at the head of this file.
  bool hasClients() const override { return true; }

  int64_t publishedCount() const override {
    return published_.load(std::memory_order_relaxed);
  }

  /// Always zero, for the same reason. `published` and the output's `frames`
  /// should therefore track each other on Windows, where on macOS they diverge
  /// whenever nothing is attached.
  int64_t skippedCount() const override { return 0; }

  std::string describe() const override {
    return "Spout sender, DirectX 11 shared texture, BGRA8, CPU copy";
  }

 private:
  mutable std::mutex mutex_;
  // spoutDX is not documented as thread-safe and this is touched from the
  // clock thread, the HTTP thread and whichever thread opened the output, so
  // every entry point holds the lock. Only close() contends, and briefly.
  mutable spoutDX sender_;
  bool open_ = false;
  VideoFormat format_;
  std::atomic<int64_t> published_{0};
};

}  // namespace

std::unique_ptr<SharedSurface> createSharedSurface() {
  return std::make_unique<SpoutSurface>();
}

const char* sharedSurfaceProtocol() { return "Spout"; }

}  // namespace weblinked
