#pragma once

#include <string>

#include <FFGLSDK.h>

#include "Controls.h"
#include "Helper.h"
#include "SyphonBridge.h"

namespace weblinked {

/// A web page as a Resolume source.
///
/// The plugin itself renders nothing: WebLinked renders the page in its own
/// process and publishes it as a Syphon source, and this draws that texture on
/// the layer. Chromium cannot be hosted in Resolume's process — `CefInitialize`
/// wants the main thread's run loop, replaces the process's signal and crash
/// handlers, and needs a `main()` to dispatch its own subprocesses — so
/// "natively inside Arena" means the *plugin* is native, not the browser. See
/// docs/07-resolume-plugin.md.
///
/// What this buys over Resolume's built-in Syphon input, which can already show
/// a WebLinked feed: the URL lives *in the composition*. It is saved with the
/// comp, it is per clip, it is deck-triggerable, and there is no second
/// application for an operator to start and keep alive.
class WebLinkedPlugin : public CFFGLPlugin {
 public:
  WebLinkedPlugin();
  ~WebLinkedPlugin() override = default;

  FFResult InitGL(const FFGLViewportStruct* viewport) override;
  FFResult DeInitGL() override;
  FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;

  FFResult SetFloatParameter(unsigned int index, float value) override;
  float GetFloatParameter(unsigned int index) override;

  char* GetTextParameter(unsigned int index) override;

  /// Text parameters must accept a set, including ones that are display-only.
  ///
  /// `instantiateGL` in the SDK sets EVERY parameter's default on a fresh
  /// instance and **deletes the instance if any set returns FF_FAIL**, and the
  /// base `CFFGLPlugin::SetTextParameter` is a stub returning exactly that. A
  /// plugin that declares a text parameter without overriding this cannot be
  /// created by any real host — while a harness driving the class directly,
  /// never going through `plugMain`, carries on passing. That is the failure
  /// this override exists to prevent, and it is why `tools/sweep.sh` drives the
  /// bundle rather than the class.
  FFResult SetTextParameter(unsigned int index, const char* value) override;

 private:
  /// Builds the shader and quad. Separate from InitGL so a context loss can
  /// rebuild without re-running the rest.
  bool buildGL();
  void destroyGL();

  /// Starts, stops and re-points the helper to match the parameters. Called
  /// once per frame; does nothing unless something actually changed.
  void reconcileHelper();

  /// The name this instance's helper publishes under: the operator's stem plus
  /// a suffix unique to this object. Two copies of the plugin on two layers
  /// would otherwise both publish "WebLinked", and each would attach to
  /// whichever the directory happened to return first.
  std::string instanceSourceName() const;

  float params_[PT_COUNT] = {};
  std::string url_;
  std::string sourceName_ = "WebLinked";

  SyphonBridge syphon_;
  Helper helper_;

  /// What the helper was last started with, so a frame that changes nothing
  /// does not restart a browser.
  std::string helperUrl_;
  std::string helperName_;
  bool helperFailed_ = false;   ///< stops retrying a missing binary every frame
  std::string helperError_;

  ffglex::FFGLShader shader_;
  GLuint vao_ = 0;
  bool glReady_ = false;
};

}  // namespace weblinked
