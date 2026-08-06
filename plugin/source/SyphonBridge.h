#pragma once

#include <memory>
#include <string>
#include <vector>

namespace weblinked {

/// A Syphon client, as a plain C++ object the FFGL plugin can hold.
///
/// The Objective-C is confined to SyphonBridge.mm so the plugin proper stays
/// C++ and ports to the Spout side without a second shape.
///
/// **Every Syphon class this links is renamed at compile time** —
/// `WLSyphonOpenGLClient`, `WLSyphonServerDirectory` and the rest, through `-D`
/// macros in CMakeLists.txt. That is not tidiness. This plugin is loaded into
/// Resolume, and Resolume already has `Syphon.framework` loaded, so a second
/// `SyphonClient` class in the same process is a duplicate Objective-C class:
/// the runtime picks one arbitrarily and the other silently does not exist.
/// Renaming keeps ours separate. The protocol itself is unchanged, because it
/// lives in string literals the preprocessor does not touch —
/// `info.v002.Syphon.ServerAnnounce` and friends still say what they always
/// said, which is what lets our client find any server including WebLinked's.
class SyphonBridge {
 public:
  SyphonBridge();
  ~SyphonBridge();

  SyphonBridge(const SyphonBridge&) = delete;
  SyphonBridge& operator=(const SyphonBridge&) = delete;

  /// Attaches to the named server, or detaches when `name` is empty. Cheap and
  /// idempotent when the name has not changed, so it is safe to call every
  /// frame with whatever the parameter currently says.
  ///
  /// Must be called with a current GL context: connecting creates a texture.
  void attach(const std::string& name);

  /// True when a server of that name was found and the connection is live.
  bool attached() const;

  /// The name currently attached to, which may lag `attach()` by a frame while
  /// the server is still being discovered.
  const std::string& attachedName() const { return attachedName_; }

  /// Acquires the current frame as a GL texture. Returns false when there is
  /// nothing to draw — no server, or none published yet — in which case the
  /// caller should leave the layer alone rather than drawing black.
  ///
  /// `target` is GL_TEXTURE_RECTANGLE, not GL_TEXTURE_2D: that is what Syphon
  /// hands out, and it means unnormalised texture coordinates in the shader.
  ///
  /// Every successful acquire must be matched by `release()` after drawing.
  bool acquire(unsigned int& texture, unsigned int& target, int& width, int& height);

  /// Releases the image acquired by `acquire()`. Safe to call when the last
  /// acquire failed.
  void release();

  /// Every Syphon server currently announcing, for the plugin's log — an
  /// operator whose name is a typo wants to see what was actually there.
  std::vector<std::string> serverNames() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string attachedName_;
};

}  // namespace weblinked
