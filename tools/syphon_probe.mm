// syphon_probe — an independent Syphon receiver, for verifying the shared
// output.
//
// Independent in the way that matters: it links **Resolume Arena's bundled
// Syphon 5 framework**, not the Syphon 6 server sources vendored into
// third_party/syphon. A pass here is two different implementations, shipped
// years apart by different people, agreeing about the protocol — not this
// repository agreeing with itself. It is also the exact client path Arena
// uses, GL texture and all.
//
// It answers the questions the server's own counters cannot:
//
//   --list          did the source announce itself, and does it answer a
//                   discovery request from a process that started *afterwards*
//   --alphabars     are the colours and the premultiplied alpha right
//                   (against tools/alphabars.html)
//   --orientation   is the picture the right way up
//                   (against tools/updown.html)
//
// The two page checks are separate because neither covers the other:
// alphabars' bands are vertical, so it reads identically through a vertical
// flip, and updown says nothing about alpha.
//
// Build (the framework path is Arena's; any app that bundles Syphon will do):
//
//   clang++ -std=c++20 -fobjc-arc tools/syphon_probe.mm \
//     -F "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" \
//     -framework Syphon -framework Foundation -framework IOSurface \
//     -framework OpenGL -framework Cocoa \
//     -rpath "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" \
//     -o syphon_probe
//
// The Syphon 5 headers are not distributed with the framework, so the classes
// used are declared here, with the selectors read off the shipped binary. They
// are long-stable public API; if a future Syphon changes them this file stops
// compiling, which is the correct failure.

#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

@interface SyphonServerDirectory : NSObject
+ (id)sharedDirectory;
- (NSArray*)servers;
@end

@interface SyphonImage : NSObject
- (GLuint)textureName;
- (NSSize)textureSize;
@end

@interface SyphonClient : NSObject
- (id)initWithServerDescription:(NSDictionary*)description
                        context:(CGLContextObj)context
                        options:(NSDictionary*)options
                newFrameHandler:(void (^)(id client))handler;
- (SyphonImage*)newFrameImage;
- (BOOL)hasNewFrame;
- (void)stop;
@end

namespace {

/// Pumps the run loop for `seconds`. Discovery is a distributed notification
/// round trip, and the frame handshake is another, so nothing arrives without
/// one.
void spin(double seconds) {
  [[NSRunLoop currentRunLoop]
      runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

const char* describe(NSDictionary* server) {
  NSString* name = server[@"SyphonServerDescriptionNameKey"];
  NSString* app = server[@"SyphonServerDescriptionAppNameKey"];
  return [[NSString stringWithFormat:@"%@ (%@)", name ?: @"—", app ?: @"—"]
      UTF8String];
}

/// A legacy profile on purpose: Syphon 5 hands back a GL_TEXTURE_RECTANGLE_ARB
/// and this only ever reads it back, never draws, so a core profile would buy
/// a VAO and nothing else.
CGLContextObj createContext() {
  const CGLPixelFormatAttribute attributes[] = {kCGLPFAAccelerated,
                                                kCGLPFAColorSize, (CGLPixelFormatAttribute)32,
                                                (CGLPixelFormatAttribute)0};
  CGLPixelFormatObj format = nullptr;
  GLint count = 0;
  if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || format == nullptr) {
    return nullptr;
  }
  CGLContextObj context = nullptr;
  CGLCreateContext(format, nullptr, &context);
  CGLDestroyPixelFormat(format);
  return context;
}

/// Reads a rectangle texture back as BGRA in memory order — row 0 first, which
/// is the row the server wrote first. That is what makes the orientation check
/// meaningful rather than circular.
///
/// Via an FBO and glReadPixels, not glGetTexImage. Syphon's texture is bound
/// to an IOSurface with CGLTexImageIOSurface2D, and glGetTexImage on one of
/// those returns zeros on this driver while reporting GL_NO_ERROR — a silent
/// wrong answer that reads exactly like a server publishing blank frames.
/// Attaching to a framebuffer is the supported way to get at those pixels.
///
/// glReadPixels starts at the framebuffer's lower left, which for a rectangle
/// texture is texel row 0, which is the first row in memory. So this does not
/// introduce a flip of its own and the orientation check stays honest.
bool readTexture(CGLContextObj context, GLuint name, int width, int height,
                 std::vector<uint8_t>& pixels) {
  CGLSetCurrentContext(context);
  pixels.assign(static_cast<size_t>(width) * height * 4, 0);

  GLuint framebuffer = 0;
  glGenFramebuffersEXT(1, &framebuffer);
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
  glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                            GL_TEXTURE_RECTANGLE_ARB, name, 0);

  const GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
  if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
    std::fprintf(stderr, "framebuffer incomplete: 0x%04x\n", status);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDeleteFramebuffersEXT(1, &framebuffer);
    return false;
  }

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
               pixels.data());
  const GLenum error = glGetError();

  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
  glDeleteFramebuffersEXT(1, &framebuffer);
  if (error != GL_NO_ERROR) {
    std::fprintf(stderr, "glReadPixels failed: 0x%04x\n", error);
    return false;
  }
  return true;
}

struct Sample {
  const char* name;
  int b, g, r, a;
};

/// Tolerance per channel. Chromium rounds its own premultiply, so 1 is the
/// floor and 2 leaves room for a rounding rule changing under us without
/// turning the check into a rubber stamp.
constexpr int kTolerance = 2;

bool check(const std::vector<uint8_t>& pixels, int width, int x, int y,
           const Sample& want) {
  const uint8_t* p = pixels.data() + ((static_cast<size_t>(y) * width + x) * 4);
  const bool ok = std::abs(p[0] - want.b) <= kTolerance &&
                  std::abs(p[1] - want.g) <= kTolerance &&
                  std::abs(p[2] - want.r) <= kTolerance &&
                  std::abs(p[3] - want.a) <= kTolerance;
  std::printf("  (%4d,%4d) %-12s got BGRA %3d %3d %3d %3d  want %3d %3d %3d %3d  %s\n",
              x, y, want.name, p[0], p[1], p[2], p[3], want.b, want.g, want.r,
              want.a, ok ? "ok" : "MISMATCH");
  return ok;
}

/// tools/alphabars.html: opaque red, 50% green, 25% blue, nothing.
///
/// Chromium premultiplies and this output does not undo that, so green arrives
/// at 192*0.5 and blue at 192*0.25. Checking against the *unpremultiplied*
/// values is the mistake this table exists to prevent.
int checkAlphaBars(const std::vector<uint8_t>& pixels, int width, int height) {
  const Sample bands[] = {
      {"opaque red", 0, 0, 192, 255},
      {"50% green", 0, 96, 0, 128},
      {"25% blue", 48, 0, 0, 64},
      {"transparent", 0, 0, 0, 0},
  };
  int failures = 0;
  const int y = height / 2;
  for (int band = 0; band < 4; ++band) {
    // Middle of each quarter, away from any edge the layout might round.
    const int x = (width * (2 * band + 1)) / 8;
    if (!check(pixels, width, x, y, bands[band])) ++failures;
  }
  return failures;
}

/// tools/updown.html: red on top, blue underneath.
///
/// Row 0 of the readback is the row the server wrote first. Red there means
/// the frame is the right way up.
int checkOrientation(const std::vector<uint8_t>& pixels, int width, int height) {
  const Sample top{"top red", 0, 0, 192, 255};
  const Sample bottom{"bottom blue", 192, 0, 0, 255};
  int failures = 0;
  if (!check(pixels, width, width / 2, height / 8, top)) ++failures;
  if (!check(pixels, width, width / 2, (height * 7) / 8, bottom)) ++failures;
  if (failures != 0) {
    std::printf("  (red below and blue above means the frame is flipped)\n");
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    std::string wanted;
    bool list = false;
    bool alphabars = false;
    bool orientation = false;
    double wait = 3.0;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--list") {
        list = true;
      } else if (arg == "--alphabars") {
        alphabars = true;
      } else if (arg == "--orientation") {
        orientation = true;
      } else if (arg == "--source" && i + 1 < argc) {
        wanted = argv[++i];
      } else if (arg == "--wait" && i + 1 < argc) {
        wait = std::atof(argv[++i]);
      } else {
        std::fprintf(stderr,
                     "usage: syphon_probe [--list] [--source <name>] "
                     "[--alphabars] [--orientation] [--wait <seconds>]\n");
        return 2;
      }
    }

    // The directory posts a discovery request when it starts. This process
    // started after the server did, so a server that only broadcast its
    // opening announce — the failure mode of creating one off the main thread
    // — would be invisible here. That is the point.
    id directory = [SyphonServerDirectory sharedDirectory];
    spin(wait);
    NSArray* servers = [directory servers];

    if (list || wanted.empty()) {
      std::printf("%lu Syphon server(s):\n", (unsigned long)servers.count);
      for (NSDictionary* server in servers) {
        std::printf("  %s\n", describe(server));
      }
      if (wanted.empty()) return servers.count > 0 ? 0 : 1;
    }

    NSDictionary* match = nil;
    NSString* target = [NSString stringWithUTF8String:wanted.c_str()];
    for (NSDictionary* server in servers) {
      if ([server[@"SyphonServerDescriptionNameKey"] isEqualToString:target]) {
        match = server;
        break;
      }
    }
    if (match == nil) {
      std::fprintf(stderr, "no Syphon server named '%s'\n", wanted.c_str());
      return 1;
    }
    std::printf("found %s\n", describe(match));

    CGLContextObj context = createContext();
    if (context == nullptr) {
      std::fprintf(stderr, "could not create a CGL context\n");
      return 1;
    }
    CGLSetCurrentContext(context);

    SyphonClient* client = [[SyphonClient alloc] initWithServerDescription:match
                                                                  context:context
                                                                  options:nil
                                                          newFrameHandler:nil];
    if (client == nil) {
      std::fprintf(stderr, "could not connect to '%s'\n", wanted.c_str());
      return 1;
    }

    // Attaching is a round trip, and the server only starts copying once it
    // sees a client. -newFrameImage will hand back an image as soon as the
    // connection exists, *before* any frame has been published into it — read
    // that one and every channel is zero, which looks exactly like a broken
    // server. So wait for -hasNewFrame, and take several frames rather than
    // the first: the earliest ones can still be in flight.
    SyphonImage* image = nil;
    int seen = 0;
    for (int attempt = 0; attempt < 400 && seen < 5; ++attempt) {
      spin(0.02);
      if (![client hasNewFrame]) {
        continue;
      }
      image = [client newFrameImage];
      if (image != nil) ++seen;
    }
    if (image == nil) {
      std::fprintf(stderr, "connected, but no frame arrived\n");
      [client stop];
      return 1;
    }
    std::printf("settled after %d published frame(s)\n", seen);

    const NSSize size = [image textureSize];
    const int width = static_cast<int>(size.width);
    const int height = static_cast<int>(size.height);
    std::printf("frame %dx%d received\n", width, height);

    std::vector<uint8_t> pixels;
    if (!readTexture(context, [image textureName], width, height, pixels)) {
      [client stop];
      return 1;
    }

    int failures = 0;
    if (alphabars) {
      std::printf("alphabars:\n");
      failures += checkAlphaBars(pixels, width, height);
    }
    if (orientation) {
      std::printf("orientation:\n");
      failures += checkOrientation(pixels, width, height);
    }
    if (alphabars || orientation) {
      std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    }

    [client stop];
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(context);
    return failures == 0 ? 0 : 1;
  }
}
