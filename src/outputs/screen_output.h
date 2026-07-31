#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "outputs/output.h"
#include "outputs/screen_window.h"

namespace weblinked {

/// A GPU-attached display, filled with the rendered page.
///
/// Modelled as an output for the same reason the preview is: it goes through
/// exactly the frame path SDI and NDI go through, so a screen that looks right
/// is evidence the others are right, and a screen that looks wrong is a bug in
/// something they share rather than in a private display path.
///
/// Deliberately *not* a second Chromium window. Every source browser here is
/// windowless, windowless rendering forces Alloy runtime style, and a windowed
/// browser defaults to Chrome style; running both in one process crashed the
/// GPU process on every attempt, which is what removed the old operator window
/// (see docs/01-architecture.md). This takes the frames the engine has already
/// produced and puts them on the glass itself.
///
/// All the interesting work is per-platform and lives behind ScreenWindow.
/// What is left here is the IOutput contract and the counters.
class ScreenOutput final : public IOutput {
 public:
  explicit ScreenOutput(const OutputSpec& spec);
  ~ScreenOutput() override;

  std::string kind() const override { return "screen"; }
  PixelFormat pixelFormat() const override { return PixelFormat::kBGRA; }

  /// A display has nowhere to put audio, and saying so lets the engine skip
  /// preparing any when a screen is the only thing running.
  bool wantsAudio() const override { return false; }

  /// Premultiplied is what a screen wants. The frame is composited over black,
  /// which is exactly what Chromium's own paint already assumes — asking for
  /// straight alpha would buy an unpremultiply pass whose result is discarded.
  bool wantsStraightAlpha() const override { return false; }

  bool start(const VideoFormat& format, std::string& error) override;
  void stop() override;
  void submit(const VideoFrame& video, const AudioBlock& audio) override;
  json::Value status() const override;

 private:
  int display_ = 0;
  ScreenScaling scaling_ = ScreenScaling::kFit;
  std::unique_ptr<ScreenWindow> window_;
  std::atomic<int64_t> submitted_{0};
};

}  // namespace weblinked
