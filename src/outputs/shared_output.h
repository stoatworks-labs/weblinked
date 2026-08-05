#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "outputs/output.h"
#include "outputs/shared_surface.h"

namespace weblinked {

/// The rendered page, published to another application on this machine as a
/// shared GPU surface — Syphon on macOS, Spout on Windows.
///
/// One output kind rather than two, for the same reason `screen` is one kind
/// across three window systems: exactly one protocol exists in any given
/// binary, so a name that says *what it does* rather than which library it
/// used keeps a settings file portable between an operator's Mac and the
/// Windows machine in the rack. `status()` reports which protocol is actually
/// in use, and the CLI accepts `--syphon=` and `--spout=` because those are
/// the words on the other application's menu.
///
/// All the interesting work is per-platform and lives behind SharedSurface.
/// What is left here is the IOutput contract and the counters.
class SharedOutput final : public IOutput {
 public:
  explicit SharedOutput(const OutputSpec& spec);
  ~SharedOutput() override;

  std::string kind() const override { return "shared"; }
  PixelFormat pixelFormat() const override { return PixelFormat::kBGRA; }

  /// Neither protocol carries audio, and saying so lets the engine skip
  /// preparing any when a shared surface is the only thing running.
  bool wantsAudio() const override { return false; }

  /// Premultiplied, which is what Chromium paints and what every consumer of
  /// these protocols expects. Asking for straight alpha would buy an
  /// unpremultiply pass that the consumer would then have to undo.
  ///
  /// Nothing here has to arrange for transparency: `OutputBackground::opaque`
  /// already defaults to false, so an operator who adds a shared output and
  /// changes nothing gets the page's own alpha on the consumer's layer. An
  /// operator who *wants* it flattened sets a background as they would for any
  /// other output, and the engine composites once for all of them.
  bool wantsStraightAlpha() const override { return false; }

  bool start(const VideoFormat& format, std::string& error) override;
  void stop() override;
  void submit(const VideoFrame& video, const AudioBlock& audio) override;
  json::Value status() const override;

 private:
  std::unique_ptr<SharedSurface> surface_;
  std::atomic<int64_t> submitted_{0};
};

}  // namespace weblinked
