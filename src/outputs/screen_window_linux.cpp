// The Linux screen output: X11 for the window, EGL + GLES2 for the picture.
//
// NEVER RUN. This compiles against Mesa's headers and has never been executed
// on any machine — see docs/04-verification.md. Treat it as intent.
//
// Structured like the Windows backend rather than the macOS one: the window and
// its GL context belong to a thread of this object's own, which also carries
// the X event loop. eglSwapInterval(1) makes eglSwapBuffers block on the
// vertical blank, so that loop is the pacing too.
//
// X11 only. A Wayland session running XWayland will get a window; a session
// without XWayland will not, and open() says so rather than failing obscurely.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "diag/diag.h"
#include "outputs/screen_frame_ring.h"
#include "outputs/screen_geometry.h"
#include "outputs/screen_window.h"

namespace weblinked {
namespace {

constexpr int kRingSlots = ScreenFrameRing::kSlots;

const char* const kVertexShader = R"GLSL(
attribute vec2 position;
uniform vec2 scale;
varying vec2 uv;
void main() {
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
  // GL texture space is bottom-left origin but the frame arrives top-down, so
  // this flip is what keeps the picture the right way up.
  vec2 base = vec2(position.x, 1.0 - position.y);
  uv = (base - 0.5) * scale + 0.5;
}
)GLSL";

const char* const kFragmentShader = R"GLSL(
precision mediump float;
uniform sampler2D frame;
varying vec2 uv;
void main() {
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  // .bgr, not .rgb. The frame is BGRA and the texture is uploaded as GL_RGBA,
  // because GLES2 only accepts BGRA with EXT_texture_format_BGRA8888 and
  // depending on an extension for a backend nobody has run is a bad trade. The
  // channel swap costs nothing here and needs no extension check.
  gl_FragColor = vec4(texture2D(frame, uv).bgr, 1.0);
}
)GLSL";


/// One X screen, as --screen indexes it.
///
/// Deliberately per-X-screen rather than per-Xinerama-head: reading the RandR
/// layout would mean another library on the link line for a backend nothing has
/// ever run. Documented in docs/03-control-api.md so the limitation is visible
/// rather than surprising.
struct LinuxDisplayEntry {
  int number = 0;
  int width = 0;
  int height = 0;
};

std::vector<LinuxDisplayEntry> collectScreens() {
  std::vector<LinuxDisplayEntry> entries;
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    return entries;
  }
  const int count = ScreenCount(display);
  for (int i = 0; i < count; ++i) {
    LinuxDisplayEntry entry;
    entry.number = i;
    entry.width = DisplayWidth(display, i);
    entry.height = DisplayHeight(display, i);
    entries.push_back(entry);
  }
  XCloseDisplay(display);
  return entries;
}

GLuint compileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

class LinuxScreenWindow final : public ScreenWindow {
 public:
  ~LinuxScreenWindow() override { LinuxScreenWindow::close(); }

  bool open(const VideoFormat& format, int display, ScreenScaling scaling,
            std::string& error) override;
  void close() override;
  void present(const VideoFrame& frame) override;

  int64_t presentedCount() const override { return ring_.presentedCount(); }
  int64_t droppedCount() const override { return ring_.droppedCount(); }
  std::string describe() const override { return description_; }

 private:
  void threadMain(VideoFormat format, int screenNumber, ScreenScaling scaling);
  bool buildContext(int screenNumber, std::string& error);
  void teardown();
  void drawOnce();

  ScreenFrameRing ring_;
  std::thread thread_;
  std::atomic<bool> quit_{false};

  std::mutex startMutex_;
  std::condition_variable startSignal_;
  bool started_ = false;
  bool startOk_ = false;
  std::string startError_;

  std::string description_ = "OpenGL ES 2.0";
  int frameWidth_ = 0;
  int frameHeight_ = 0;
  int displayWidth_ = 0;
  int displayHeight_ = 0;
  ScreenScale uniforms_;

  Display* xDisplay_ = nullptr;
  Window xWindow_ = 0;
  EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
  EGLContext eglContext_ = EGL_NO_CONTEXT;
  EGLSurface eglSurface_ = EGL_NO_SURFACE;
  GLuint program_ = 0;
  GLuint texture_ = 0;
  GLuint vertexBuffer_ = 0;
  GLint scaleLocation_ = -1;

  std::vector<uint8_t> slots_[kRingSlots];
};

bool LinuxScreenWindow::open(const VideoFormat& format, int display,
                             ScreenScaling scaling, std::string& error) {
  if (thread_.joinable()) {
    return true;
  }
  frameWidth_ = format.width;
  frameHeight_ = format.height;
  for (auto& slot : slots_) {
    slot.assign(static_cast<size_t>(format.width) * format.height * 4, 0);
  }

  quit_.store(false);
  started_ = false;
  startOk_ = false;
  startError_.clear();

  thread_ = std::thread(&LinuxScreenWindow::threadMain, this, format, display,
                        scaling);

  std::unique_lock<std::mutex> lock(startMutex_);
  startSignal_.wait(lock, [this] { return started_; });
  if (!startOk_) {
    error = startError_;
    lock.unlock();
    close();
    return false;
  }
  return true;
}

void LinuxScreenWindow::close() {
  ring_.shutdown();
  quit_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void LinuxScreenWindow::present(const VideoFrame& frame) {
  if (frame.pixelFormat() != PixelFormat::kBGRA) {
    return;
  }
  if (frame.format().width != frameWidth_ || frame.format().height != frameHeight_) {
    return;
  }
  const int slot = ring_.claim();
  if (slot < 0) {
    return;
  }
  const size_t rowBytes = static_cast<size_t>(frameWidth_) * 4;
  const uint8_t* source = frame.data();
  uint8_t* destination = slots_[slot].data();
  for (int y = 0; y < frameHeight_; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * rowBytes,
                source + static_cast<size_t>(y) * frame.rowBytes(), rowBytes);
  }
  ring_.publish(slot);
}

void LinuxScreenWindow::threadMain(VideoFormat format, int screenNumber,
                                   ScreenScaling scaling) {
  const auto signalStart = [this](bool ok, std::string message) {
    std::lock_guard<std::mutex> lock(startMutex_);
    started_ = true;
    startOk_ = ok;
    startError_ = std::move(message);
    startSignal_.notify_all();
  };

  std::string error;
  if (!buildContext(screenNumber, error)) {
    teardown();
    signalStart(false, error);
    return;
  }

  uniforms_ = screenScaleFor(format.width, format.height, displayWidth_,
                             displayHeight_, scaling);
  ring_.reset();
  signalStart(true, {});

  while (!quit_.load()) {
    while (XPending(xDisplay_) > 0) {
      XEvent event;
      XNextEvent(xDisplay_, &event);
      // Nothing to handle: the window is override-redirect, fullscreen and
      // takes no input. Events are drained so the connection does not back up.
    }
    drawOnce();
  }

  teardown();
}

bool LinuxScreenWindow::buildContext(int screenNumber, std::string& error) {
  // Every X call from this point runs on this thread only, so XInitThreads is
  // not needed — but CEF may already have opened its own connection, which is
  // exactly why this opens a separate one rather than sharing.
  xDisplay_ = XOpenDisplay(nullptr);
  if (xDisplay_ == nullptr) {
    error = "cannot open an X display (no DISPLAY, or a Wayland session without XWayland)";
    return false;
  }
  if (screenNumber < 0 || screenNumber >= ScreenCount(xDisplay_)) {
    error = "display " + std::to_string(screenNumber) + " does not exist (" +
            std::to_string(ScreenCount(xDisplay_)) + " X screens)";
    return false;
  }

  displayWidth_ = DisplayWidth(xDisplay_, screenNumber);
  displayHeight_ = DisplayHeight(xDisplay_, screenNumber);
  const Window root = RootWindow(xDisplay_, screenNumber);

  XSetWindowAttributes attributes{};
  attributes.background_pixel = BlackPixel(xDisplay_, screenNumber);
  // override_redirect keeps the window manager from reparenting, decorating or
  // resizing what is meant to be an exact-size fullscreen output.
  attributes.override_redirect = True;
  attributes.event_mask = ExposureMask | StructureNotifyMask;
  xWindow_ = XCreateWindow(
      xDisplay_, root, 0, 0, static_cast<unsigned int>(displayWidth_),
      static_cast<unsigned int>(displayHeight_), 0, CopyFromParent, InputOutput,
      CopyFromParent, CWBackPixel | CWOverrideRedirect | CWEventMask, &attributes);
  if (xWindow_ == 0) {
    error = "could not create the output window";
    return false;
  }
  XMapRaised(xDisplay_, xWindow_);
  XFlush(xDisplay_);

  eglDisplay_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(xDisplay_));
  if (eglDisplay_ == EGL_NO_DISPLAY || eglInitialize(eglDisplay_, nullptr, nullptr) == EGL_FALSE) {
    error = "could not initialise EGL";
    return false;
  }

  const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint configCount = 0;
  if (eglChooseConfig(eglDisplay_, configAttributes, &config, 1, &configCount) == EGL_FALSE ||
      configCount == 0) {
    error = "no suitable EGL config";
    return false;
  }

  eglSurface_ = eglCreateWindowSurface(
      eglDisplay_, config, static_cast<EGLNativeWindowType>(xWindow_), nullptr);
  if (eglSurface_ == EGL_NO_SURFACE) {
    error = "could not create an EGL surface";
    return false;
  }

  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  eglBindAPI(EGL_OPENGL_ES_API);
  eglContext_ = eglCreateContext(eglDisplay_, config, EGL_NO_CONTEXT, contextAttributes);
  if (eglContext_ == EGL_NO_CONTEXT ||
      eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) == EGL_FALSE) {
    error = "could not create an EGL context";
    return false;
  }
  // The whole pacing story: swap blocks until the vertical blank.
  eglSwapInterval(eglDisplay_, 1);

  const GLubyte* renderer = glGetString(GL_RENDERER);
  if (renderer != nullptr) {
    description_ = std::string("OpenGL ES 2.0, ") +
                   reinterpret_cast<const char*>(renderer);
  }

  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vertexShader == 0 || fragmentShader == 0) {
    error = "the screen output's shaders would not compile";
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vertexShader);
  glAttachShader(program_, fragmentShader);
  glBindAttribLocation(program_, 0, "position");
  glLinkProgram(program_);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);
  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    error = "the screen output's shader program would not link";
    return false;
  }
  scaleLocation_ = glGetUniformLocation(program_, "scale");

  // GLES2 has no gl_VertexID, so the fullscreen triangle needs a real buffer.
  const float vertices[] = {0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f};
  glGenBuffers(1, &vertexBuffer_);
  glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // Uploaded as GL_RGBA and read back as .bgr in the fragment shader; see the
  // note there for why this does not use GL_BGRA_EXT.
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameWidth_, frameHeight_, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  return true;
}

void LinuxScreenWindow::teardown() {
  if (eglDisplay_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglContext_ != EGL_NO_CONTEXT) {
      eglDestroyContext(eglDisplay_, eglContext_);
      eglContext_ = EGL_NO_CONTEXT;
    }
    if (eglSurface_ != EGL_NO_SURFACE) {
      eglDestroySurface(eglDisplay_, eglSurface_);
      eglSurface_ = EGL_NO_SURFACE;
    }
    eglTerminate(eglDisplay_);
    eglDisplay_ = EGL_NO_DISPLAY;
  }
  if (xDisplay_ != nullptr) {
    if (xWindow_ != 0) {
      XDestroyWindow(xDisplay_, xWindow_);
      xWindow_ = 0;
    }
    XCloseDisplay(xDisplay_);
    xDisplay_ = nullptr;
  }
}

void LinuxScreenWindow::drawOnce() {
  const int slot = ring_.acquire();
  glViewport(0, 0, displayWidth_, displayHeight_);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (slot < 0) {
    eglSwapBuffers(eglDisplay_, eglSurface_);
    return;
  }

  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameWidth_, frameHeight_, GL_RGBA,
                  GL_UNSIGNED_BYTE, slots_[slot].data());

  glUseProgram(program_);
  glUniform2f(scaleLocation_, uniforms_.x, uniforms_.y);
  glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  const EGLBoolean swapped = eglSwapBuffers(eglDisplay_, eglSurface_);
  ring_.release(swapped == EGL_TRUE);
}

}  // namespace

std::unique_ptr<ScreenWindow> createScreenWindow() {
  return std::make_unique<LinuxScreenWindow>();
}

std::vector<DisplayInfo> enumerateDisplays() {
  std::vector<DisplayInfo> displays;
  const auto screens = collectScreens();
  for (size_t i = 0; i < screens.size(); ++i) {
    DisplayInfo info;
    info.index = static_cast<int>(i);
    info.name = ":" + std::to_string(screens[i].number);
    info.width = screens[i].width;
    info.height = screens[i].height;
    // X does not report a refresh rate without RandR, which this backend
    // deliberately does not link. Left at 0 rather than invented.
    info.refreshHz = 0;
    info.primary = i == 0;
    displays.push_back(std::move(info));
  }
  return displays;
}

}  // namespace weblinked
