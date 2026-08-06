#include "WebLinkedPlugin.h"

#include <cstdint>
#include <cstdio>

namespace weblinked {
namespace {

/// A full-screen triangle strip generated from gl_VertexID — no vertex buffer,
/// because there is nothing to store. The VAO still has to exist: a core
/// profile refuses to draw without one bound, even when no attributes are read.
const char* const kVertexShader = R"GLSL(#version 410 core
out vec2 uv;
void main()
{
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    // Flipped vertically: clip space counts up the screen and the Syphon
    // surface's first row is the top of the page. Getting this wrong is the
    // one mistake that still looks plausible on a symmetrical test pattern,
    // which is why tools/updown.html exists on the WebLinked side.
    uv = vec2(pos.x, 1.0 - pos.y);
}
)GLSL";

/// `sampler2DRect`, not `sampler2D`: Syphon hands out a GL_TEXTURE_RECTANGLE,
/// whose coordinates are in texels rather than 0..1. Hence the multiply by the
/// texture size rather than a plain lookup.
const char* const kFragmentShader = R"GLSL(#version 410 core
uniform sampler2DRect Picture;
uniform vec2 PictureSize;
in vec2 uv;
out vec4 fragColor;
void main()
{
    fragColor = texture(Picture, uv * PictureSize);
}
)GLSL";

}  // namespace

WebLinkedPlugin::WebLinkedPlugin() {
  // A source: it generates a picture rather than filtering one.
  SetMinInputs(0);
  SetMaxInputs(0);

  SetParamInfo(PT_URL, "URL", FF_TYPE_TEXT, "");

  SetOptionParamInfo(PT_MODE, "Source", 2, 0.0f);
  SetParamElementInfo(PT_MODE, 0, "Run WebLinked", 0.0f);
  SetParamElementInfo(PT_MODE, 1, "Attach", 1.0f);

  SetParamInfo(PT_SOURCE_NAME, "Source Name", FF_TYPE_TEXT, sourceName_.c_str());
  SetParamInfo(PT_RUN, "Run", FF_TYPE_BOOLEAN, true);
  params_[PT_RUN] = 1.0f;
}

std::string WebLinkedPlugin::instanceSourceName() const {
  // The object's own address, which is unique among live instances and stable
  // for this one's lifetime. Not persisted anywhere and not meant to be: the
  // name only has to be unique while the helper is running, and a composition
  // reload makes a new instance and a new helper anyway.
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "-%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this) & 0xffffff));
  return sourceName_ + suffix;
}

void WebLinkedPlugin::reconcileHelper() {
  const bool wantHelper = modeFromParam(params_[PT_MODE]) == SourceMode::kRun &&
                          params_[PT_RUN] >= 0.5f && !url_.empty();

  if (!wantHelper) {
    if (!helperUrl_.empty() || helper_.alive()) {
      helper_.stop();
      helperUrl_.clear();
      helperName_.clear();
    }
    // Cleared so that turning Run back on, or fixing the binary, retries.
    helperFailed_ = false;
    return;
  }

  const std::string wanted = instanceSourceName();

  // A helper that died — a page that crashed Chromium, or an operator killing
  // it — is restarted rather than left as a black layer. This is the whole
  // reason the browser is out of process: it can die without taking the show
  // with it, so long as something notices.
  if (!helper_.alive()) {
    helperUrl_.clear();
    helperName_.clear();
  }

  if (helperName_ == wanted && helperUrl_ == url_) {
    return;  // nothing changed
  }

  if (helperName_ == wanted && helper_.alive()) {
    // Only the URL moved. Re-point the running page through the control API
    // instead of restarting it: a restart costs a browser launch and a black
    // layer, and an operator editing a URL sees every intermediate keystroke.
    if (helper_.setUrl(url_)) {
      helperUrl_ = url_;
      return;
    }
    // The helper is alive but would not take it. Fall through and restart.
  }

  if (helperFailed_) {
    return;  // do not spawn-fail once per frame
  }

  std::string error;
  if (!helper_.start(url_, wanted, error)) {
    helperFailed_ = true;
    helperError_ = error;
    helperUrl_.clear();
    helperName_.clear();
    return;
  }
  helperUrl_ = url_;
  helperName_ = wanted;
}

bool WebLinkedPlugin::buildGL() {
  if (glReady_) {
    return true;
  }
  if (!shader_.Compile(kVertexShader, kFragmentShader)) {
    return false;
  }
  glGenVertexArrays(1, &vao_);
  if (vao_ == 0) {
    shader_.FreeGLResources();
    return false;
  }
  glReady_ = true;
  return true;
}

void WebLinkedPlugin::destroyGL() {
  if (vao_ != 0) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  shader_.FreeGLResources();
  glReady_ = false;
}

FFResult WebLinkedPlugin::InitGL(const FFGLViewportStruct* viewport) {
  (void)viewport;
  return buildGL() ? FF_SUCCESS : FF_FAIL;
}

FFResult WebLinkedPlugin::DeInitGL() {
  // The helper first: a browser outliving the layer that owns it is the one
  // failure an operator cannot see and cannot clean up, because nothing on
  // screen refers to it any more.
  helper_.stop();
  helperUrl_.clear();
  helperName_.clear();

  // Before the shader, because detaching touches the context the client was
  // created against.
  syphon_.release();
  destroyGL();
  return FF_SUCCESS;
}

FFResult WebLinkedPlugin::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
  (void)pGL;
  if (!glReady_ && !buildGL()) {
    return FF_FAIL;
  }

  // Nothing happens until an operator has typed a URL. During Resolume's
  // plugin scan every plugin is instantiated with its defaults, and an empty
  // URL is what keeps that from launching a browser per installed plugin.
  reconcileHelper();

  std::string attachTo;
  if (params_[PT_RUN] >= 0.5f) {
    attachTo = modeFromParam(params_[PT_MODE]) == SourceMode::kRun
                   ? helperName_          // empty until the helper is up
                   : sourceName_;
  }
  syphon_.attach(attachTo);

  unsigned int texture = 0;
  unsigned int target = 0;
  int width = 0;
  int height = 0;
  if (!syphon_.acquire(texture, target, width, height)) {
    // No server, or no frame published yet. Leave the layer as Resolume left
    // it rather than drawing black — a plugin that cleared here would flash on
    // every composition load and every helper restart.
    return FF_SUCCESS;
  }

  // Plain glUseProgram and glBindTexture rather than the ffglex Scoped*
  // helpers: every one of those clears its binding to 0 on scope exit instead
  // of restoring what was there. State is put back by hand below.
  glBindVertexArray(vao_);
  glUseProgram(shader_.GetGLID());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(target, texture);

  shader_.Set("Picture", 0);
  shader_.Set("PictureSize", static_cast<float>(width), static_cast<float>(height));

  // The page's alpha is premultiplied — Chromium paints that way and the
  // WebLinked shared output deliberately does not undo it — which is exactly
  // what Resolume's own blend expects. Nothing to convert.
  glDisable(GL_BLEND);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glBindTexture(target, 0);
  glUseProgram(0);
  glBindVertexArray(0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  syphon_.release();
  return FF_SUCCESS;
}

FFResult WebLinkedPlugin::SetFloatParameter(unsigned int index, float value) {
  if (index >= PT_COUNT) {
    return FF_FAIL;
  }
  params_[index] = value;
  return FF_SUCCESS;
}

float WebLinkedPlugin::GetFloatParameter(unsigned int index) {
  return index < PT_COUNT ? params_[index] : 0.0f;
}

char* WebLinkedPlugin::GetTextParameter(unsigned int index) {
  // const_cast because the SDK's signature is non-const. The host reads the
  // pointer and does not write through it; both strings are members, so they
  // outlive the call and stay at a stable address.
  if (index == PT_URL) {
    return const_cast<char*>(url_.c_str());
  }
  if (index == PT_SOURCE_NAME) {
    return const_cast<char*>(sourceName_.c_str());
  }
  return nullptr;
}

FFResult WebLinkedPlugin::SetTextParameter(unsigned int index, const char* value) {
  const std::string text = (value != nullptr) ? value : "";
  if (index == PT_URL) {
    url_ = text;
    return FF_SUCCESS;
  }
  if (index == PT_SOURCE_NAME) {
    // An empty name would attach to nothing and give an operator a black layer
    // with no explanation, so it falls back rather than being accepted as-is.
    sourceName_ = text.empty() ? "WebLinked" : text;
    return FF_SUCCESS;
  }
  // Must not be FF_FAIL for an index we do not use: see the header. Returning
  // failure for any parameter during instantiateGL destroys the instance.
  return FF_SUCCESS;
}

}  // namespace weblinked
