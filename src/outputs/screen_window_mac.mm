// The macOS screen output: AppKit for the window, Metal for the picture,
// CVDisplayLink for the pacing.
//
// Three things about this file are not obvious and cost real time if assumed:
//
// 1. There is no ARC. CMakeLists deliberately avoids enable_language(OBJCXX)
//    (see the comment at its head) and nothing adds -fobjc-arc, so every .mm
//    here is manual retain/release. Objects are owned where they are created
//    and released in close(); anything autoreleased on the display-link thread
//    needs its own @autoreleasepool, because that thread has none.
//
// 2. Deployment target is 12.0 (Info.plist.in, LSMinimumSystemVersion), so the
//    macOS 14 display-link APIs are not available. CVDisplayLink is deprecated
//    as of macOS 15 but is the one that works across the supported range, and
//    the deprecation is silenced narrowly below rather than project-wide.
//
// 3. The layer's colour space is pinned to sRGB. Left alone, macOS colour-
//    manages from the display's own profile and the picture no longer matches
//    the reference the NDI output is checked against — which turns a colour
//    verification into a wild goose chase through the conversion code.

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "diag/diag.h"
#include "outputs/screen_frame_ring.h"
#include "outputs/screen_geometry.h"
#include "outputs/screen_window.h"

namespace weblinked {
namespace {

constexpr int kRingSlots = ScreenFrameRing::kSlots;

const char* const kShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms { float2 scale; };
struct VertexOut { float4 position [[position]]; float2 uv; };

// One oversized triangle rather than a quad: no vertex buffer, no index buffer,
// and the GPU clips it to the viewport for free.
vertex VertexOut vertexMain(uint vid [[vertex_id]],
                            constant Uniforms& u [[buffer(0)]]) {
  float2 pos = float2(float((vid << 1) & 2), float(vid & 2));
  VertexOut out;
  out.position = float4(pos * 2.0 - 1.0, 0.0, 1.0);
  // Metal textures are top-left origin while clip space is bottom-left, so the
  // vertical flip happens here rather than by uploading the frame upside down.
  float2 uv = float2(pos.x, 1.0 - pos.y);
  out.uv = (uv - 0.5) * u.scale + 0.5;
  return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> tex [[texture(0)]],
                             sampler samp [[sampler(0)]]) {
  // Outside the picture is the letterbox. Explicit black rather than relying on
  // clamp-to-edge, which would smear the edge pixels across the bars.
  if (in.uv.x < 0.0 || in.uv.x > 1.0 || in.uv.y < 0.0 || in.uv.y > 1.0) {
    return float4(0.0, 0.0, 0.0, 1.0);
  }
  // Chromium paints premultiplied and this composites over black, so the
  // premultiplied RGB is already the correct result. Alpha is forced opaque:
  // the window must not let the desktop show through.
  return float4(tex.sample(samp, in.uv).rgb, 1.0);
}
)METAL";

/// Runs `block` on the main thread and waits.
///
/// start() arrives on the main thread at boot but on the HTTP thread when an
/// operator adds an output from the control page, and AppKit will not create a
/// window from the latter. The isMainThread test is not an optimisation — a
/// dispatch_sync to the main queue *from* the main thread deadlocks outright.
void runOnMain(void (^block)(void)) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

NSScreen* screenAtIndex(int index) {
  NSArray<NSScreen*>* screens = [NSScreen screens];
  if (index < 0 || static_cast<NSUInteger>(index) >= [screens count]) {
    return nil;
  }
  return [screens objectAtIndex:static_cast<NSUInteger>(index)];
}

CGDirectDisplayID displayIdForScreen(NSScreen* screen) {
  NSNumber* number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
  return static_cast<CGDirectDisplayID>([number unsignedIntValue]);
}


class MacScreenWindow final : public ScreenWindow {
 public:
  ~MacScreenWindow() override { MacScreenWindow::close(); }

  bool open(const VideoFormat& format, int display, ScreenScaling scaling,
            std::string& error) override;
  void close() override;
  void present(const VideoFrame& frame) override;

  int64_t presentedCount() const override { return ring_.presentedCount(); }
  int64_t droppedCount() const override { return ring_.droppedCount(); }
  std::string describe() const override { return description_; }

  /// Called from the CVDisplayLink thread.
  void draw();

 private:
  ScreenFrameRing ring_;

  // AppKit and Metal objects. All owned here under manual retain/release.
  NSWindow* window_ = nil;
  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> pipeline_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  id<MTLBuffer> slotBuffers_[kRingSlots] = {nil, nil, nil};
  id<MTLTexture> slotTextures_[kRingSlots] = {nil, nil, nil};
  CVDisplayLinkRef link_ = nullptr;

  // Layout-compatible with the shader's `float2 scale`.
  ScreenScale uniforms_;
  std::string description_ = "Metal";

  int frameWidth_ = 0;
  int frameHeight_ = 0;
  size_t alignedRowBytes_ = 0;
  bool open_ = false;
};

CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                             CVOptionFlags, CVOptionFlags*, void* context) {
  // This thread is not the main thread and has no autorelease pool of its own,
  // so every autoreleased Metal object below would leak once per refresh —
  // about 200 000 of them an hour at 60 Hz.
  @autoreleasepool {
    static_cast<MacScreenWindow*>(context)->draw();
  }
  return kCVReturnSuccess;
}

bool MacScreenWindow::open(const VideoFormat& format, int display,
                           ScreenScaling scaling, std::string& error) {
  if (open_) {
    return true;
  }

  __block bool ok = false;
  __block std::string failure;

  runOnMain(^{
    NSScreen* screen = screenAtIndex(display);
    if (screen == nil) {
      failure = "display " + std::to_string(display) + " does not exist (" +
                std::to_string(static_cast<int>([[NSScreen screens] count])) +
                " attached)";
      return;
    }

    const CGDirectDisplayID displayId = displayIdForScreen(screen);

    // The GPU actually driving that head, which on a laptop with an external
    // monitor is not necessarily the default device.
    device_ = CGDirectDisplayCopyCurrentMetalDevice(displayId);
    if (device_ == nil) {
      device_ = MTLCreateSystemDefaultDevice();
    }
    if (device_ == nil) {
      failure = "no Metal device for display " + std::to_string(display);
      return;
    }

    // No extra retain on any of the new* calls below: Metal follows the Cocoa
    // naming rule, so they already come back owned (+1). Retaining as well
    // would leak the device, the queue and every buffer in the ring.
    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) {
      failure = "could not create a Metal command queue";
      return;
    }

    NSError* compileError = nil;
    NSString* source = [NSString stringWithUTF8String:kShaderSource];
    id<MTLLibrary> library = [device_ newLibraryWithSource:source
                                                   options:nil
                                                     error:&compileError];
    if (library == nil) {
      failure = std::string("Metal shader would not compile: ") +
                [[compileError localizedDescription] UTF8String];
      return;
    }

    MTLRenderPipelineDescriptor* descriptor =
        [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    descriptor.vertexFunction =
        [[library newFunctionWithName:@"vertexMain"] autorelease];
    descriptor.fragmentFunction =
        [[library newFunctionWithName:@"fragmentMain"] autorelease];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    [library release];

    pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                        error:&compileError];
    if (pipeline_ == nil) {
      failure = std::string("Metal pipeline would not build: ") +
                [[compileError localizedDescription] UTF8String];
      return;
    }

    MTLSamplerDescriptor* samplerDescriptor =
        [[[MTLSamplerDescriptor alloc] init] autorelease];
    // Linear, because a 1080p page on a 4K head is the normal case and nearest
    // would alias badly. Clamp keeps the edge from wrapping into the letterbox.
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:samplerDescriptor];

    // ---- the frame ring -----------------------------------------------------
    //
    // Each slot is one shared-storage MTLBuffer with a texture *aliasing* its
    // memory. That is what keeps this to a single copy: present() memcpys the
    // frame straight into GPU-visible memory and the draw samples it in place,
    // where an ordinary texture upload would cost a second copy per frame.
    frameWidth_ = format.width;
    frameHeight_ = format.height;
    const size_t alignment =
        [device_ minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
    const size_t tightRowBytes = static_cast<size_t>(format.width) * 4;
    alignedRowBytes_ = alignment == 0
                           ? tightRowBytes
                           : ((tightRowBytes + alignment - 1) / alignment) * alignment;

    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:static_cast<NSUInteger>(format.width)
                                    height:static_cast<NSUInteger>(format.height)
                                 mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageShaderRead;

    for (int i = 0; i < kRingSlots; ++i) {
      slotBuffers_[i] = [device_ newBufferWithLength:alignedRowBytes_ * format.height
                                             options:MTLResourceStorageModeShared];
      if (slotBuffers_[i] == nil) {
        failure = "could not allocate a Metal frame buffer";
        return;
      }
      std::memset([slotBuffers_[i] contents], 0, alignedRowBytes_ * format.height);
      slotTextures_[i] = [slotBuffers_[i] newTextureWithDescriptor:textureDescriptor
                                                            offset:0
                                                       bytesPerRow:alignedRowBytes_];
      if (slotTextures_[i] == nil) {
        failure = "could not alias a Metal texture onto the frame buffer";
        return;
      }
    }

    // ---- the window ---------------------------------------------------------
    const NSRect frame = [screen frame];
    // screen:nil, then setFrame: — and that is not a stylistic choice.
    //
    // initWithContentRect:...screen: interprets the rect relative to the origin
    // of the screen it is given, while -[NSScreen frame] is in *global*
    // coordinates. Passing one to the other therefore applies the screen's
    // offset twice, and every display except the main one — whose origin is
    // (0,0), so the two agree — gets a window somewhere off the desktop.
    //
    // The failure is silent and deeply misleading: open() succeeds, the
    // CVDisplayLink is created on the correct display and ticks at that
    // display's exact refresh rate, Metal draws, and `presented` climbs at
    // 60/s against a 50 Hz source — every counter says it is working, and
    // nothing is on any screen. Found only by screenshotting both displays and
    // discovering the picture on neither.
    //
    // -setFrame:display: is unambiguous: always global coordinates.
    window_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, frame.size.width,
                                                               frame.size.height)
                                          styleMask:NSWindowStyleMaskBorderless
                                            backing:NSBackingStoreBuffered
                                              defer:NO
                                             screen:nil];
    [window_ setFrame:frame display:YES];
    // NOT optional, and not obvious. isReleasedWhenClosed defaults to YES for a
    // window built with initWithContentRect:, so -close would release it and
    // the -release in close() below becomes an over-release. That does not
    // fault where it happens: the object is already gone, and the process dies
    // later inside objc_autoreleasePoolPop on the main thread, which reads as a
    // CEF bug. Cost an add/remove cycle to find — see docs/04-verification.md.
    [window_ setReleasedWhenClosed:NO];
    [window_ setOpaque:YES];
    [window_ setBackgroundColor:[NSColor blackColor]];
    // Above the menu bar and the Dock, but below a screen saver.
    [window_ setLevel:NSMainMenuWindowLevel + 1];
    [window_ setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                   NSWindowCollectionBehaviorStationary |
                                   NSWindowCollectionBehaviorFullScreenNone |
                                   NSWindowCollectionBehaviorIgnoresCycle];
    // This is a video output, not a user interface. Letting it swallow clicks
    // would leave an operator unable to reach anything underneath a window that
    // covers a whole display — and the real UI is the control page anyway.
    [window_ setIgnoresMouseEvents:YES];

    const CGFloat scale = [screen backingScaleFactor];
    const int pixelWidth = static_cast<int>(frame.size.width * scale);
    const int pixelHeight = static_cast<int>(frame.size.height * scale);

    layer_ = [[CAMetalLayer layer] retain];
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    layer_.opaque = YES;
    layer_.contentsScale = scale;
    layer_.drawableSize = CGSizeMake(pixelWidth, pixelHeight);
    // See the note at the head of this file: without this the picture is
    // colour-managed to the display profile and stops matching the reference.
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer_.colorspace = colorSpace;
    CGColorSpaceRelease(colorSpace);

    // Order matters. setLayer: before setWantsLayer: makes the view
    // layer-*hosting* — ours is the layer. The other way round AppKit creates
    // a backing layer of its own first and the Metal layer becomes a child it
    // is free to resize and reposition.
    NSView* view = [window_ contentView];
    layer_.frame = [view bounds];
    [view setLayer:layer_];
    [view setWantsLayer:YES];

    // orderFrontRegardless, not makeKeyAndOrderFront: — a render host must not
    // steal focus from whatever the operator is actually working in.
    [window_ orderFrontRegardless];

    uniforms_ = screenScaleFor(format.width, format.height, pixelWidth,
                               pixelHeight, scaling);

    description_ = std::string("Metal, ") + [[device_ name] UTF8String];

    // ---- pacing -------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (CVDisplayLinkCreateWithCGDisplay(displayId, &link_) != kCVReturnSuccess) {
      failure = "could not create a display link for display " +
                std::to_string(display);
      return;
    }
    CVDisplayLinkSetOutputCallback(link_, &displayLinkCallback, this);
    // Opened before the link starts, or the first few callbacks find a closed
    // ring and the display stays black until a frame happens to land after it.
    ring_.reset();
    CVDisplayLinkStart(link_);
#pragma clang diagnostic pop

    ok = true;
  });

  if (!ok) {
    error = failure;
    close();  // Releases whatever did get built before the failure.
    return false;
  }

  open_ = true;
  return true;
}

void MacScreenWindow::close() {
  // Closed first, so a draw already inside the callback finds nothing to
  // acquire and returns rather than starting work we are about to free.
  ring_.shutdown();

  // Stopped before anything is released. CVDisplayLinkStop waits for a callback
  // already in flight to return, which is the only thing keeping draw() from
  // running against half-released Metal objects.
  if (link_ != nullptr) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(link_);
    CVDisplayLinkRelease(link_);
#pragma clang diagnostic pop
    link_ = nullptr;
  }

  runOnMain(^{
    if (window_ != nil) {
      [window_ orderOut:nil];
      [window_ close];
      [window_ release];
      window_ = nil;
    }
    [layer_ release];
    layer_ = nil;
    for (int i = 0; i < kRingSlots; ++i) {
      [slotTextures_[i] release];
      slotTextures_[i] = nil;
      [slotBuffers_[i] release];
      slotBuffers_[i] = nil;
    }
    [sampler_ release];
    sampler_ = nil;
    [pipeline_ release];
    pipeline_ = nil;
    [queue_ release];
    queue_ = nil;
    [device_ release];
    device_ = nil;
  });

  open_ = false;
}

void MacScreenWindow::present(const VideoFrame& frame) {
  if (frame.pixelFormat() != PixelFormat::kBGRA) {
    return;  // The engine honours pixelFormat(); this is belt and braces.
  }
  if (frame.format().width != frameWidth_ || frame.format().height != frameHeight_) {
    // A paint from before a raster change. Dropping it is correct — the ring
    // was allocated at the old size and copying into it would overrun.
    return;
  }
  const int slot = ring_.claim();
  if (slot < 0) {
    return;
  }

  // Row by row: the source is tightly packed but the destination is padded up
  // to Metal's linear-texture alignment, so a single memcpy would shear the
  // picture on any width whose row bytes are not already a multiple of it.
  const uint8_t* source = frame.data();
  const size_t sourceRowBytes = static_cast<size_t>(frame.rowBytes());
  uint8_t* destination = static_cast<uint8_t*>([slotBuffers_[slot] contents]);
  const size_t copyBytes = std::min(sourceRowBytes, alignedRowBytes_);
  for (int y = 0; y < frameHeight_; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * alignedRowBytes_,
                source + static_cast<size_t>(y) * sourceRowBytes, copyBytes);
  }

  ring_.publish(slot);
}

void MacScreenWindow::draw() {
  const int slot = ring_.acquire();
  if (slot < 0) {
    return;
  }

  // nextDrawable returns nil when the layer has no free drawable — normal
  // under load, and not a frame that reached the glass, so release() is told.
  id<CAMetalDrawable> drawable = [layer_ nextDrawable];
  if (drawable != nil) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = [drawable texture];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline_];
    [encoder setVertexBytes:&uniforms_ length:sizeof(uniforms_) atIndex:0];
    [encoder setFragmentTexture:slotTextures_[slot] atIndex:0];
    [encoder setFragmentSamplerState:sampler_ atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
  }

  ring_.release(drawable != nil);
}

}  // namespace

std::unique_ptr<ScreenWindow> createScreenWindow() {
  return std::make_unique<MacScreenWindow>();
}

std::vector<DisplayInfo> enumerateDisplays() {
  std::vector<DisplayInfo> displays;
  @autoreleasepool {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    // Index 0 is the screen carrying the menu bar, which is what an operator
    // means by "the main display" and what --screen defaults to.
    NSScreen* main = [screens count] > 0 ? [screens objectAtIndex:0] : nil;
    for (NSUInteger i = 0; i < [screens count]; ++i) {
      NSScreen* screen = [screens objectAtIndex:i];
      const NSRect frame = [screen frame];
      const CGFloat scale = [screen backingScaleFactor];

      DisplayInfo info;
      info.index = static_cast<int>(i);
      info.name = [[screen localizedName] UTF8String];
      info.width = static_cast<int>(frame.size.width * scale);
      info.height = static_cast<int>(frame.size.height * scale);
      info.primary = screen == main;

      const CGDirectDisplayID displayId = displayIdForScreen(screen);
      CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
      if (mode != nullptr) {
        info.refreshHz = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
      }
      // Built-in panels report 0 here. Left as 0 rather than guessed at: a
      // made-up 60 in the control page would be worse than an honest blank.

      displays.push_back(std::move(info));
    }
  }
  return displays;
}

}  // namespace weblinked
