cmake_minimum_required(VERSION 3.20)
project(weblinked_plugin VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# A separate CMake project on purpose, exactly as launcher/ is a separate Cargo
# project. It does NOT build with the rest of WebLinked and must not be added
# to the top-level CMakeLists: it needs the FFGL SDK, it produces a Resolume
# bundle rather than an application, and it has no CEF anywhere in it. Build it
# from this directory.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal so one bundle loads in both an Apple Silicon and an Intel Resolume.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  # Before any target, and before project() has finished mattering — set later
  # and the result is silently arm64-only.
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
  message(FATAL_ERROR
    "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# No Syphon is vendored, linked or compiled into this bundle. On purpose, and
# it is the most important line in this file.
#
# The first attempt vendored Syphon's client sources with every class renamed
# through -D macros, to avoid colliding with the Syphon.framework Resolume
# already has loaded. The renaming worked. Resolume crashed anyway, merely
# scanning the plugin folder — inside its OWN Syphon, in objc method lookup,
# with our bundle already absent from the crash report's image list.
#
# Resolume dlcloses a plugin after inspecting it, and Syphon's sources add
# categories to Foundation classes. Those attach to NSArray and NSDictionary,
# which outlive the bundle; unloading left their method lists pointing into
# unmapped memory. Renaming classes does nothing about that — a category lands
# on its target whatever the contributing image is called, and an Objective-C
# image that has extended somebody else's class is not safely unloadable.
#
# So the plugin contributes no Objective-C metadata at all and borrows the host's
# Syphon through NSClassFromString. See the header of source/SyphonBridge.mm.
# If you are tempted to add an Objective-C class, category or protocol to this
# target, read that first, then check with:
#
#     otool -o WebLinked.bundle/Contents/MacOS/WebLinked | grep -c "class name"
# ---------------------------------------------------------------------------
# Everything except the registration, as an OBJECT library.
#
# OBJECT and not STATIC on purpose: CFFGLPluginInfo in SourcePlugin.cpp
# registers itself from a file-scope constructor and nothing references it by
# name, so in an archive the linker may drop the translation unit entirely. The
# result is a bundle that loads, exports plugMain, and contains no plugins.
# ---------------------------------------------------------------------------
add_library(weblinked_plugin_core OBJECT
  source/Helper.cpp
  source/Helper.h
  source/WebLinkedPlugin.cpp
  source/WebLinkedPlugin.h
  source/Controls.h
  source/SyphonBridge.h)

if(APPLE)
  target_sources(weblinked_plugin_core PRIVATE source/SyphonBridge.mm)

  # NOT ARC. The bridge reaches Syphon through typed objc_msgSend casts, and
  # ARC cannot reason about ownership through a cast — it would either
  # over-release what -newFrameImage hands back or leak the client. The
  # retain/release in that file is by hand and deliberate.
  set_source_files_properties(source/SyphonBridge.mm PROPERTIES
    COMPILE_OPTIONS "-fno-objc-arc")

  # OpenGL is deprecated on macOS and is still the only API FFGL speaks, so the
  # warning is noise on every line that draws anything.
  target_compile_definitions(weblinked_plugin_core PUBLIC GL_SILENCE_DEPRECATION)

  # Foundation and OpenGL only. No AppKit (nothing here touches NSWorkspace any
  # more) and no Syphon: the classes are looked up at run time from whatever the
  # host has already loaded.
  target_link_libraries(weblinked_plugin_core PUBLIC
    "-framework Foundation" "-framework OpenGL")
endif()

target_include_directories(weblinked_plugin_core PUBLIC source)
target_link_libraries(weblinked_plugin_core PUBLIC ffgl::sdk)

# ---------------------------------------------------------------------------
# The bundle.
# ---------------------------------------------------------------------------
add_library(WebLinkedPlugin MODULE source/SourcePlugin.cpp)
target_link_libraries(WebLinkedPlugin PRIVATE weblinked_plugin_core)

if(APPLE)
  set_target_properties(WebLinkedPlugin PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "bundle"
    OUTPUT_NAME "WebLinked"
    MACOSX_BUNDLE_BUNDLE_NAME "WebLinked"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.weblinked"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
else()
  set_target_properties(WebLinkedPlugin PROPERTIES OUTPUT_NAME "WebLinked")
endif()
