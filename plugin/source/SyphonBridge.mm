// The Syphon client, as seen from C++.
//
// ---------------------------------------------------------------------------
// THIS FILE DEFINES NO OBJECTIVE-C CLASSES, CATEGORIES OR PROTOCOLS, AND THAT
// IS THE WHOLE POINT. Do not add any.
// ---------------------------------------------------------------------------
//
// The first version of this plugin vendored Syphon's own client sources, with
// every class renamed through -D macros so as not to collide with the
// Syphon.framework Resolume already has loaded. The renaming worked — no
// duplicate classes — and Resolume crashed anyway, hard, while merely scanning
// the plugin folder:
//
//     EXC_BAD_ACCESS in getMethodNoSuper_nolock
//       <- objc_msgSend_uncached
//       <- +[NSArray arrayWithArray:]
//       <- -[SyphonServerDirectory servers]      (Arena's Syphon, not ours)
//
// Resolume `dlclose`s a plugin bundle after inspecting it. Our bundle was gone
// from the crash report's image list entirely — but Syphon's sources add
// *categories to Foundation classes* (`NSArray (SyphonServerDirectoryServerSearch)`,
// `NSDictionary (SyphonServerDirectoryPimpMyDictionary)`), and those attach to
// NSArray and NSDictionary, which outlive us. Unloading left their method
// lists pointing into unmapped memory, so the next time Arena's *own* Syphon
// enumerated servers the runtime walked a dangling list and died.
//
// Renaming classes does nothing about this. Categories land on the target
// class no matter what the contributing image is called, and an Objective-C
// image is not safely unloadable once it has extended somebody else's class.
//
// So this plugin contributes no metadata to the runtime at all. It borrows the
// Syphon that Resolume already has, reached through NSClassFromString and
// typed objc_msgSend casts. No @interface (that would emit an undefined
// _OBJC_CLASS_$_ reference and force us to link a Syphon), no @protocol, no
// category, nothing for dlclose to leave behind.
//
// The API used is Syphon 5's, which is what Resolume bundles, and every
// selector below was read off the shipped binary rather than assumed.
//
// Compiled WITHOUT ARC on purpose: ARC cannot reason about ownership through
// an objc_msgSend cast, so the retain/release here is by hand and deliberate.

#include "SyphonBridge.h"

#import <Foundation/Foundation.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <objc/message.h>
#import <objc/runtime.h>

namespace weblinked {
namespace {

/// Typed sends. Each cast names the real signature, because objc_msgSend must
/// be called through a function pointer of the callee's exact type — calling
/// it unprototyped is undefined on arm64 and wrong for struct returns.
id sendId(id target, const char* selector) {
  if (target == nil) return nil;
  using Fn = id (*)(id, SEL);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector));
}

BOOL sendBool(id target, const char* selector) {
  if (target == nil) return NO;
  using Fn = BOOL (*)(id, SEL);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector));
}

void sendVoid(id target, const char* selector) {
  if (target == nil) return;
  using Fn = void (*)(id, SEL);
  reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector));
}

/// The one class we need to find rather than be handed. Returns nil in a host
/// that has no Syphon, which is a black layer rather than a crash.
Class syphonClass(const char* name) { return NSClassFromString(@(name)); }

}  // namespace

struct SyphonBridge::Impl {
  id client = nil;  ///< SyphonClient, retained
  id image = nil;   ///< SyphonImage, retained
};

SyphonBridge::SyphonBridge() : impl_(std::make_unique<Impl>()) {}

SyphonBridge::~SyphonBridge() {
  release();
  if (impl_->client != nil) {
    sendVoid(impl_->client, "stop");
    [impl_->client release];
    impl_->client = nil;
  }
}

void SyphonBridge::attach(const std::string& name) {
  if (name == attachedName_ && impl_->client != nil &&
      sendBool(impl_->client, "isValid")) {
    return;
  }

  release();
  if (impl_->client != nil) {
    // -stop touches the GL context the client was created against, which is
    // why attach() is documented as needing a current one.
    sendVoid(impl_->client, "stop");
    [impl_->client release];
    impl_->client = nil;
  }
  attachedName_.clear();

  if (name.empty()) {
    return;
  }

  Class directoryClass = syphonClass("SyphonServerDirectory");
  Class clientClass = syphonClass("SyphonClient");
  if (directoryClass == nil || clientClass == nil) {
    // No Syphon in this host. Nothing to attach to, and nothing to complain
    // about every frame either.
    return;
  }

  id directory = sendId(reinterpret_cast<id>(directoryClass), "sharedDirectory");
  NSArray* servers = static_cast<NSArray*>(sendId(directory, "servers"));

  NSString* wanted = [NSString stringWithUTF8String:name.c_str()];
  NSDictionary* match = nil;
  for (NSDictionary* server in servers) {
    // Matched on the server's own name rather than the application's: that is
    // the half an operator types, and it is exactly WebLinked's --syphon=<name>.
    if ([server[@"SyphonServerDescriptionNameKey"] isEqualToString:wanted]) {
      match = server;
      break;
    }
  }
  if (match == nil) {
    return;
  }

  CGLContextObj context = CGLGetCurrentContext();
  if (context == nullptr) {
    return;
  }

  // No new-frame handler: the host already calls us once per composition
  // frame, so a callback would only add a second clock and a threading
  // problem.
  using InitFn = id (*)(id, SEL, id, CGLContextObj, id, id);
  id allocated = sendId(reinterpret_cast<id>(clientClass), "alloc");
  id client = reinterpret_cast<InitFn>(objc_msgSend)(
      allocated, sel_registerName("initWithServerDescription:context:options:newFrameHandler:"),
      match, context, nil, nil);

  if (client == nil || !sendBool(client, "isValid")) {
    [client release];
    return;
  }
  impl_->client = client;  // +1 from alloc/init, released in detach
  attachedName_ = name;
}

bool SyphonBridge::attached() const {
  return impl_->client != nil && sendBool(impl_->client, "isValid");
}

bool SyphonBridge::acquire(unsigned int& texture, unsigned int& target, int& width,
                           int& height) {
  release();
  if (!attached()) {
    return false;
  }

  // -newFrameImage hands back an image as soon as the connection exists, before
  // the server has published into it — read that one and every channel is zero,
  // which looks exactly like a broken server. It resolves itself within a frame
  // or two and until then this simply draws nothing. It cost real debugging
  // time on the receiver side; see docs/04-verification.md section 23.
  id image = sendId(impl_->client, "newFrameImage");
  if (image == nil) {
    return false;
  }
  impl_->image = [image retain];  // -newFrameImage is a +1 already; balanced in release()
  [image release];

  using SizeFn = NSSize (*)(id, SEL);
  const NSSize size =
      reinterpret_cast<SizeFn>(objc_msgSend)(impl_->image, sel_registerName("textureSize"));
  if (size.width <= 0 || size.height <= 0) {
    release();
    return false;
  }

  using NameFn = GLuint (*)(id, SEL);
  texture = reinterpret_cast<NameFn>(objc_msgSend)(impl_->image,
                                                   sel_registerName("textureName"));
  // Syphon hands out a rectangle texture, not a 2D one — hence unnormalised
  // coordinates in the shader.
  target = GL_TEXTURE_RECTANGLE;
  width = static_cast<int>(size.width);
  height = static_cast<int>(size.height);
  return texture != 0;
}

void SyphonBridge::release() {
  if (impl_->image != nil) {
    [impl_->image release];
    impl_->image = nil;
  }
}

std::vector<std::string> SyphonBridge::serverNames() const {
  std::vector<std::string> names;
  Class directoryClass = syphonClass("SyphonServerDirectory");
  if (directoryClass == nil) {
    return names;
  }
  id directory = sendId(reinterpret_cast<id>(directoryClass), "sharedDirectory");
  NSArray* servers = static_cast<NSArray*>(sendId(directory, "servers"));
  for (NSDictionary* server in servers) {
    NSString* name = server[@"SyphonServerDescriptionNameKey"];
    if (name != nil) {
      names.emplace_back([name UTF8String]);
    }
  }
  return names;
}

}  // namespace weblinked
