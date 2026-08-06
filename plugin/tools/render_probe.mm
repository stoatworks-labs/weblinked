// render_probe — does the plugin actually draw the page?
//
// The question Resolume would answer by putting it on a layer and looking. This
// answers it in a way that can be checked rather than eyeballed, and without a
// GUI: it drives the built bundle through `plugMain` exactly as a host does —
// instantiate, set the URL, render — into an offscreen framebuffer, then reads
// the pixels back and compares them against what tools/alphabars.html is known
// to paint.
//
// It exercises the whole chain in one go: the helper is spawned, WebLinked
// renders the page, publishes it over Syphon, the plugin's client picks it up,
// and the rectangle-texture shader draws it. A wrong sampler, a missing flip, a
// premultiply that got undone somewhere, or a helper that never started all
// show up here as wrong numbers.
//
// Build: plugin/tools/build_unload_probe.sh
// Run:   WEBLINKED_BINARY=/path/to/WebLinked ./out/render_probe

#import <Foundation/Foundation.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using FFUInt32 = unsigned int;
union FFMixed {
  FFUInt32 UIntValue;
  void* PointerValue;
};
using FFInstanceID = void*;
using PlugMainFn = FFMixed (*)(FFUInt32, FFMixed, FFInstanceID);

struct FFGLViewportStruct {
  unsigned int x, y, width, height;
};
struct SetParameterStruct {
  FFUInt32 ParameterNumber;
  FFMixed NewParameterValue;
};
struct ProcessOpenGLStruct {
  FFUInt32 numInputTextures;
  void** inputTextures;
  GLuint HostFBO;
};

constexpr FFUInt32 kSetParameter = 8;
constexpr FFUInt32 kProcessOpenGL = 17;
constexpr FFUInt32 kInstantiateGL = 18;
constexpr FFUInt32 kDeinstantiateGL = 19;
constexpr FFUInt32 kFFFail = 0xFFFFFFFF;

// Parameter indices, mirroring plugin/source/Controls.h. Asserted by name at
// run time below rather than trusted, so a reordering is caught here instead of
// silently setting the wrong thing.
constexpr FFUInt32 kParamUrl = 0;
constexpr FFUInt32 kParamMode = 1;
constexpr FFUInt32 kParamSourceName = 2;
constexpr FFUInt32 kParamRun = 3;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

int failures = 0;
void check(bool condition, const char* what) {
  std::printf("  %-52s %s\n", what, condition ? "ok" : "FAIL");
  if (!condition) ++failures;
}

void spin(double seconds) {
  [[NSRunLoop currentRunLoop]
      runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

CGLContextObj createContext() {
  const CGLPixelFormatAttribute attributes[] = {
      kCGLPFAAccelerated,
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
      (CGLPixelFormatAttribute)0};
  CGLPixelFormatObj format = nullptr;
  GLint count = 0;
  if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format) {
    return nullptr;
  }
  CGLContextObj context = nullptr;
  CGLCreateContext(format, nullptr, &context);
  CGLDestroyPixelFormat(format);
  return context;
}

struct Band {
  const char* name;
  int r, g, b, a;
};

/// tools/alphabars.html, premultiplied — the same table the Syphon and Spout
/// verifications use, so the numbers are directly comparable.
constexpr Band kBands[4] = {
    {"opaque red", 192, 0, 0, 255},
    {"50% green", 0, 96, 0, 128},
    {"25% blue", 0, 0, 48, 64},
    {"transparent", 0, 0, 0, 0},
};

/// Generous next to the exact matches elsewhere, and deliberately so: this
/// pixel has been through Chromium's premultiply, an IOSurface, a Syphon
/// texture and a GLSL sampler. Two code values of slack catches a wrong
/// channel, a missing alpha or a flip while forgiving filtering.
constexpr int kTolerance = 2;

}  // namespace

int main() {
  @autoreleasepool {
    const char* binary = std::getenv("WEBLINKED_BINARY");
    std::printf("WEBLINKED_BINARY: %s\n", binary ? binary : "(unset — will look in /Applications)");

    const std::string bundlePath =
        std::string(getenv("HOME")) +
        "/Projects/weblinked/plugin/build/WebLinked.bundle/Contents/MacOS/WebLinked";

    void* handle = dlopen(bundlePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
      return 1;
    }
    auto plugMain = reinterpret_cast<PlugMainFn>(dlsym(handle, "plugMain"));
    if (!plugMain) {
      std::fprintf(stderr, "no plugMain\n");
      return 1;
    }

    CGLContextObj context = createContext();
    if (!context) {
      std::fprintf(stderr, "no GL context\n");
      return 1;
    }
    CGLSetCurrentContext(context);

    // A texture and FBO for the plugin to draw into, exactly as a host capturing
    // a layer would provide.
    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      std::fprintf(stderr, "framebuffer incomplete\n");
      return 1;
    }
    glViewport(0, 0, kWidth, kHeight);

    std::printf("\ninstantiate:\n");
    FFGLViewportStruct viewport{0, 0, kWidth, kHeight};
    FFMixed argument{};
    argument.PointerValue = &viewport;
    FFMixed instance = plugMain(kInstantiateGL, argument, nullptr);
    const bool created = instance.UIntValue != kFFFail && instance.PointerValue;
    check(created, "instantiateGL succeeded");
    if (!created) return 1;
    FFInstanceID id = instance.PointerValue;

    // Set the URL. This is the moment the plugin is allowed to spawn anything —
    // before it, the URL is empty and the plugin must stay inert.
    std::printf("\nset parameters:\n");
    const std::string url =
        "file://" + std::string(getenv("HOME")) +
        "/Projects/weblinked/tools/alphabars.html";
    SetParameterStruct set{};
    set.ParameterNumber = kParamUrl;
    set.NewParameterValue.PointerValue = const_cast<char*>(url.c_str());
    FFMixed setArg{};
    setArg.PointerValue = &set;
    check(plugMain(kSetParameter, setArg, id).UIntValue != kFFFail, "URL accepted");

    // Mode 0 = Run WebLinked, Run = on. Both are the defaults; set explicitly
    // so the test does not depend on them staying that way.
    SetParameterStruct mode{};
    mode.ParameterNumber = kParamMode;
    mode.NewParameterValue.UIntValue = 0;
    FFMixed modeArg{};
    modeArg.PointerValue = &mode;
    (void)plugMain(kSetParameter, modeArg, id);

    std::printf("\nrender:\n");
    ProcessOpenGLStruct process{};
    process.numInputTextures = 0;
    process.inputTextures = nullptr;
    process.HostFBO = fbo;
    FFMixed processArg{};
    processArg.PointerValue = &process;

    // Chromium has to start, load the page and publish before there is
    // anything to draw — and "something arrived" is not "the page arrived".
    // WebLinked publishes as soon as it has a frame, and the first frames are
    // a blank document: sampling on the first non-magenta picture reads black
    // and fails a plugin that is working. That is the same mistake
    // syphon_probe made with -newFrameImage, in a new place.
    //
    // So this waits for the picture to go *steady*: the four sample points
    // unchanged across kStableFrames consecutive renders. Steady is not the
    // same as correct, which is the point — it settles on whatever the plugin
    // actually produces and the assertions below then judge it. Waiting until
    // the pixels match would be a test that cannot fail.
    constexpr int kStableFrames = 10;
    std::vector<unsigned char> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
    auto samplePoints = [&](unsigned char out[16]) {
      const int y = kHeight / 2;
      for (int band = 0; band < 4; ++band) {
        const int x = (kWidth * (2 * band + 1)) / 8;
        const unsigned char* p =
            pixels.data() + ((static_cast<size_t>(y) * kWidth + x) * 4);
        for (int c = 0; c < 4; ++c) out[band * 4 + c] = p[c];
      }
    };

    unsigned char current[16] = {0}, previous[16] = {0};
    bool drew = false;
    int stable = 0;
    int frame = 0;
    int settledOn = -1;
    for (; frame < 600; ++frame) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      // A colour the page never contains, so "something was drawn" cannot be
      // confused with "the clear survived".
      glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      (void)plugMain(kProcessOpenGL, processArg, id);
      glFinish();

      glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      samplePoints(current);

      const bool magenta =
          current[0] == 255 && current[1] == 0 && current[2] == 255;
      if (!magenta) {
        if (!drew) {
          drew = true;
          std::printf("  (first picture on frame %d)\n", frame);
        }
        stable = (std::memcmp(current, previous, sizeof(current)) == 0) ? stable + 1 : 0;
        if (stable >= kStableFrames) {
          settledOn = frame;
          break;
        }
      }
      std::memcpy(previous, current, sizeof(current));
      spin(0.05);
    }
    check(drew, "the plugin drew something");
    check(settledOn >= 0, "the picture settled to a steady state");
    if (settledOn >= 0) std::printf("  (steady from frame %d)\n", settledOn);

    if (drew) {
      // glReadPixels reads bottom-up, so row 0 of the buffer is the BOTTOM of
      // the image. alphabars is four vertical bands, which is why the row does
      // not matter here — but it is why this probe cannot judge orientation,
      // and tools/updown.html on the WebLinked side is what does.
      std::printf("\nalphabars through the plugin:\n");
      const int y = kHeight / 2;
      for (int band = 0; band < 4; ++band) {
        const int x = (kWidth * (2 * band + 1)) / 8;
        const unsigned char* p =
            pixels.data() + ((static_cast<size_t>(y) * kWidth + x) * 4);
        const Band& want = kBands[band];
        const bool ok = std::abs(p[0] - want.r) <= kTolerance &&
                        std::abs(p[1] - want.g) <= kTolerance &&
                        std::abs(p[2] - want.b) <= kTolerance &&
                        std::abs(p[3] - want.a) <= kTolerance;
        std::printf("  (%4d,%4d) %-12s got RGBA %3d %3d %3d %3d  want %3d %3d %3d %3d  %s\n",
                    x, y, want.name, p[0], p[1], p[2], p[3], want.r, want.g,
                    want.b, want.a, ok ? "ok" : "MISMATCH");
        if (!ok) ++failures;
      }
    }

    std::printf("\nteardown:\n");
    (void)plugMain(kDeinstantiateGL, FFMixed{}, id);
    check(true, "deinstantiateGL returned");

    // The helper must not outlive the instance that owned it.
    //
    // `Frameworks` is excluded on purpose. A running WebLinked is six
    // processes, not one: the browser plus five CEF subprocesses living in
    // .../Contents/Frameworks/WebLinked Helper.app/Contents/MacOS/…, which
    // match a naive "MacOS/WebLinked" and outlive their parent by a moment.
    // Counting those made this check fail against a plugin that was shutting
    // down perfectly well. What matters is the process Helper::start spawned.
    //
    // Polled rather than slept: a clean shutdown takes as long as Chromium
    // takes, and a fixed wait is either flaky or slow.
    const char* command =
        "pgrep -fl 'Contents/MacOS/WebLinked ' | grep -v Frameworks | wc -l";
    int count = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
      FILE* pipe = popen(command, "r");
      count = -1;
      if (pipe) {
        (void)std::fscanf(pipe, "%d", &count);
        pclose(pipe);
      }
      if (count == 0) break;
      spin(0.1);
    }
    check(count == 0, "no WebLinked process left behind");

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(context);
    dlclose(handle);

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
  }
}
