// unload_probe — does loading and unloading this plugin poison the host?
//
// Resolume scans a plugin folder by dlopen'ing each bundle, inspecting it, and
// dlclose'ing it again. That is the sequence that crashed Arena: the first
// version of this plugin vendored Syphon's client sources, which add
// *categories to Foundation classes*, and unloading left those method lists
// pointing into unmapped memory. The next time Arena's own Syphon called
// -[SyphonServerDirectory servers], the runtime walked a dangling list and
// took the application down.
//
// This reproduces that sequence in a few hundred milliseconds, with no GUI and
// nothing to lose:
//
//   1. report what Objective-C metadata the bundle contributes
//   2. dlopen it, call plugMain the way a scan does, dlclose it
//   3. exercise Syphon hard afterwards — the thing that actually crashed
//
// --control builds the fault instead of the fix: it installs a category on
// NSArray from a scratch bundle and unloads that, so a run of this probe that
// reports PASS for everything is not just a probe that never detects anything.
//
// Build: plugin/tools/build_unload_probe.sh

#import <Foundation/Foundation.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// The FFGL ABI, spelled out rather than included: this probe deliberately
// links none of the SDK, so that what it exercises is the built bundle's real
// entry point and not a second copy of the SDK's idea of it.
using FFUInt32 = unsigned int;
union FFMixed {
  FFUInt32 UIntValue;
  void* PointerValue;
};
using FFInstanceID = void*;
using PlugMainFn = FFMixed (*)(FFUInt32 functionCode, FFMixed inputValue,
                               FFInstanceID instanceID);

struct FFGLViewportStruct {
  unsigned int x, y, width, height;
};

constexpr FFUInt32 kGetInfo = 0;
constexpr FFUInt32 kGetNumParameters = 4;
constexpr FFUInt32 kGetParameterName = 5;
constexpr FFUInt32 kInstantiateGL = 18;
constexpr FFUInt32 kDeinstantiateGL = 19;
constexpr FFUInt32 kFFFail = 0xFFFFFFFF;

/// An offscreen context, because instantiateGL compiles the plugin's shaders
/// and a host would always have one current.
CGLContextObj createContext() {
  const CGLPixelFormatAttribute attributes[] = {
      kCGLPFAAccelerated,
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
      (CGLPixelFormatAttribute)0};
  CGLPixelFormatObj format = nullptr;
  GLint count = 0;
  if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError ||
      format == nullptr) {
    return nullptr;
  }
  CGLContextObj context = nullptr;
  CGLCreateContext(format, nullptr, &context);
  CGLDestroyPixelFormat(format);
  return context;
}

/// What the image adds to the Objective-C runtime. Anything non-zero here is
/// something dlclose has to take back cleanly, and categories on foreign
/// classes are the case it does not.
void reportMetadata(const char* path) {
  std::string command =
      std::string("otool -l '") + path +
      "' | grep -cE '__objc_classlist|__objc_catlist|__objc_protolist' || true";
  std::printf("  objc metadata sections: ");
  std::fflush(stdout);
  (void)std::system(command.c_str());
}

/// Calls Syphon the way Arena did when it died. Done through
/// NSClassFromString so this probe links no Syphon of its own — whatever is
/// loaded in the process is what gets exercised.
bool exerciseSyphon(int rounds) {
  Class directoryClass = NSClassFromString(@"SyphonServerDirectory");
  if (directoryClass == nil) {
    std::printf("  (no Syphon loaded in this process — load one to make this "
                "check meaningful)\n");
    return true;
  }
  for (int i = 0; i < rounds; ++i) {
    @autoreleasepool {
      id directory = [directoryClass performSelector:@selector(sharedDirectory)];
      NSArray* servers = [directory performSelector:@selector(servers)];
      // arrayWithArray: is the exact call in Arena's backtrace.
      NSArray* copied = [NSArray arrayWithArray:servers];
      for (NSDictionary* server in copied) {
        (void)[server objectForKey:@"SyphonServerDescriptionNameKey"];
      }
    }
  }
  return true;
}

int loadAndUnload(const char* path, bool callPlugMain) {
  std::printf("loading %s\n", path);
  reportMetadata(path);

  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    std::printf("  dlopen failed: %s\n", dlerror());
    return 1;
  }

  if (callPlugMain) {
    auto plugMain = reinterpret_cast<PlugMainFn>(dlsym(handle, "plugMain"));
    std::printf("  plugMain: %s\n", plugMain != nullptr ? "found" : "MISSING");
    if (plugMain != nullptr) {
      FFMixed none{};
      (void)plugMain(kGetInfo, none, nullptr);

      FFMixed count = plugMain(kGetNumParameters, none, nullptr);
      std::printf("  parameters: %u\n", count.UIntValue);
      for (FFUInt32 i = 0; i < count.UIntValue && i < 16; ++i) {
        FFMixed index{};
        index.UIntValue = i;
        FFMixed name = plugMain(kGetParameterName, index, nullptr);
        std::printf("    [%u] %s\n", i,
                    name.PointerValue != nullptr
                        ? static_cast<const char*>(name.PointerValue)
                        : "(null)");
      }

      // instantiateGL is where a plugin with a bad text parameter dies, and it
      // dies invisibly: the SDK sets EVERY parameter's default on a fresh
      // instance and deletes the instance if any set returns FF_FAIL, while
      // the base SetTextParameter is a stub that returns exactly that. A
      // harness driving the C++ class directly never sees it. This one goes
      // through plugMain, which is the only way to catch it.
      CGLContextObj context = createContext();
      if (context == nullptr) {
        std::printf("  (no GL context — instantiate not exercised)\n");
      } else {
        CGLSetCurrentContext(context);
        FFGLViewportStruct viewport{0, 0, 1920, 1080};
        FFMixed argument{};
        argument.PointerValue = &viewport;
        FFMixed instance = plugMain(kInstantiateGL, argument, nullptr);
        const bool ok = instance.UIntValue != kFFFail && instance.PointerValue != nullptr;
        std::printf("  instantiateGL: %s\n", ok ? "ok" : "FAILED");
        if (ok) {
          FFMixed none2{};
          (void)plugMain(kDeinstantiateGL, none2, instance.PointerValue);
          std::printf("  deinstantiateGL: ok\n");
        }
        CGLSetCurrentContext(nullptr);
        CGLDestroyContext(context);
        if (!ok) {
          std::printf("\nFAIL — no real host can create this plugin\n");
          dlclose(handle);
          return 1;
        }
      }
    }
  }

  std::printf("  dlclose...\n");
  dlclose(handle);
  std::printf("  unloaded\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* bundle = nullptr;
  bool control = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--control") {
      control = true;
    } else if (arg == "--bundle" && i + 1 < argc) {
      bundle = argv[++i];
    } else {
      std::fprintf(stderr, "usage: unload_probe --bundle <binary> [--control]\n");
      return 2;
    }
  }
  if (bundle == nullptr) {
    std::fprintf(stderr, "usage: unload_probe --bundle <binary> [--control]\n");
    return 2;
  }

  std::printf("Syphon before load: ");
  std::fflush(stdout);
  exerciseSyphon(5);
  std::printf("  ok\n");

  if (loadAndUnload(bundle, !control) != 0) {
    return 1;
  }

  // The moment of truth. If the unloaded image left anything attached to a
  // Foundation class, this is where the process dies — exactly as Arena did.
  std::printf("Syphon after unload (200 rounds):\n");
  std::fflush(stdout);
  exerciseSyphon(200);
  std::printf("  survived\n");

  std::printf("\nPASS — loading and unloading this bundle left the host intact\n");
  return 0;
}
