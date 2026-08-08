# WebLinked — a URL in, SDI and video-over-IP out.
#
# CEF dictates a lot of this file. Its binary distribution ships its own CMake
# bootstrap (cef_variables.cmake / cef_macros.cmake) which sets the C++
# standard, the macOS deployment target, the required compiler and linker flags
# and the platform library list. We adopt those rather than fight them, because
# every flag mismatch against a prebuilt Chromium shows up as a link error or,
# worse, as a crash inside CefInitialize.

cmake_minimum_required(VERSION 3.21)

# Must precede project(). A "universal" macOS build that sets this afterwards
# silently produces a single-arch binary.
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  # The CEF binary distribution is per-architecture, so we build for whatever
  # this distribution is. Universal builds need two distributions and a lipo
  # pass — see docs/02-building.md.
  set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "")
endif()

project(weblinked VERSION 0.7.1 LANGUAGES C CXX)

# Deliberately NOT enable_language(OBJCXX), even though src/app/mac_application.mm
# is Objective-C++. Enabling it makes CMake compile every .mm as OBJCXX —
# including CEF's own, inside libcef_dll_wrapper — and CEF only ever sets the CXX
# flags, so its cef_scoped_sandbox_context_mac.mm fails with "unknown type name
# 'constexpr'". Left alone, CMake compiles .mm as CXX and clang infers
# Objective-C++ from the extension, which is exactly how CEF builds its own.
#
# This only shows up in a clean build: an incremental one has the CEF wrapper
# objects already cached and never recompiles them.

set(WEBLINKED_VERSION "${PROJECT_VERSION}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(MSVC)
  # CEF's wrapper is built against the *static* runtime (/MT), so everything
  # linked beside it must be too, or the link fails with a wall of LNK2038
  # "mismatch detected for RuntimeLibrary: MT_StaticRelease doesn't match
  # MD_DynamicRelease". Setting the property rather than rewriting CMAKE_CXX_FLAGS
  # is what makes it reach libcef_dll_wrapper as well, since that comes in via
  # add_subdirectory below — and it has to be set before any target exists.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
  # NOMINMAX is not optional here. <windows.h> defines max() and min() as
  # macros, and CEF's own cef_ref_counted.h calls std::numeric_limits<>::max() —
  # so the moment a translation unit sees both, the macro eats the call and MSVC
  # reports "C2589: '(': illegal token on right side of '::'" pointing inside a
  # CEF header, which looks like a broken SDK rather than a macro collision.
  # WIN32_LEAN_AND_MEAN keeps the rest of the Windows surface out of the way.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------

option(WEBLINKED_WITH_NDI      "Build the NDI output backend"      ON)
option(WEBLINKED_WITH_OMT      "Build the OMT output backend"      ON)
option(WEBLINKED_WITH_DECKLINK "Build the DeckLink output backend" ON)
option(WEBLINKED_WITH_AJA      "Build the AJA NTV2 output backend" OFF)
option(WEBLINKED_WITH_SCREEN   "Build the fullscreen GPU output backend" ON)
option(WEBLINKED_WITH_SHARED   "Build the Syphon/Spout shared-surface output backend" ON)
option(WEBLINKED_BUILD_TESTS   "Build the unit tests"              ON)

# Syphon on macOS, Spout on Windows. Linux has no equivalent either of them
# would recognise — there is no protocol to target, not merely no code — so it
# disables itself there rather than failing the build, the same way the screen
# output does for a missing X11.
if(WEBLINKED_WITH_SHARED AND NOT APPLE AND NOT WIN32)
  message(STATUS
    "Shared-surface output: disabled — Syphon is macOS-only and Spout is "
    "Windows-only, and Linux has no equivalent to target")
  set(WEBLINKED_WITH_SHARED OFF)
endif()

# The screen output is the one backend with no SDK to find: macOS and Windows
# both ship everything it needs. Linux does not — X11 and EGL are separate dev
# packages — so it disables itself there rather than failing the build, the same
# way a missing NDI or DeckLink SDK does. See docs/02-building.md for the apt
# line that turns it back on.
if(WEBLINKED_WITH_SCREEN AND UNIX AND NOT APPLE)
  find_package(X11)
  find_library(WEBLINKED_EGL_LIBRARY NAMES EGL)
  find_library(WEBLINKED_GLES_LIBRARY NAMES GLESv2)
  if(NOT X11_FOUND OR NOT WEBLINKED_EGL_LIBRARY OR NOT WEBLINKED_GLES_LIBRARY)
    message(STATUS
      "Screen output: disabled — needs libx11-dev, libegl1-mesa-dev and "
      "libgles2-mesa-dev")
    set(WEBLINKED_WITH_SCREEN OFF)
  endif()
endif()

# ----------------------------------------------------------------------------
# CEF
# ----------------------------------------------------------------------------

include(FetchCEF)
weblinked_provision_cef()   # sets CEF_ROOT, downloading the distribution if needed

# The binary distribution ships its own FindCEF.cmake; it has to be on the module
# path before find_package can see it.
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")

# CEF's own default is USE_SANDBOX=ON, which links cef_sandbox.a — a file the
# *minimal* binary distribution does not contain. Since the whole application is
# an offscreen renderer started with no_sandbox, turning it off here is both
# necessary and honest. Must precede find_package(CEF), which reads it.
set(USE_SANDBOX OFF CACHE BOOL "Link the CEF sandbox library" FORCE)

find_package(CEF REQUIRED)
add_subdirectory("${CEF_ROOT}/libcef_dll" libcef_dll_wrapper)

# Windows and Linux link libcef directly; macOS does not, and must not.
#
# On macOS the CEF framework is loaded at run time by CefScopedLibraryLoader —
# that is the pattern the binary distribution is built for, and it is why the
# bundle works with the framework inside Contents/Frameworks. Everywhere else
# the loader has no equivalent and the symbols have to come from libcef at link
# time; without this, Linux fails with a wall of "undefined reference to
# cef_string_utf16_clear" and friends, which reads like a broken CEF build
# rather than a missing library.
if(NOT OS_MAC)
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

PRINT_CEF_CONFIG()

# ----------------------------------------------------------------------------
# Optional output SDKs
#
# Every one of these is optional at *build* time and, for NDI and OMT, loaded
# at *run* time as well: neither library is ours to redistribute, and a machine
# with no DeckLink driver should still run the NDI path.
# ----------------------------------------------------------------------------

set(WEBLINKED_BACKENDS "")

if(WEBLINKED_WITH_NDI)
  find_package(NDI)
  if(NDI_FOUND)
    list(APPEND WEBLINKED_BACKENDS ndi)
  else()
    message(STATUS "NDI SDK not found — NDI backend disabled")
    set(WEBLINKED_WITH_NDI OFF)
  endif()
endif()

if(WEBLINKED_WITH_OMT)
  find_package(OMT)
  if(OMT_FOUND)
    list(APPEND WEBLINKED_BACKENDS omt)
  else()
    message(STATUS "OMT SDK not found — OMT backend disabled (see cmake/FindOMT.cmake)")
    set(WEBLINKED_WITH_OMT OFF)
  endif()
endif()

if(WEBLINKED_WITH_DECKLINK)
  find_package(DeckLinkSDK)
  if(DeckLinkSDK_FOUND)
    list(APPEND WEBLINKED_BACKENDS decklink)
  else()
    message(STATUS "DeckLink SDK not found — DeckLink backend disabled. "
                   "Pass -DDECKLINK_SDK_DIR=/path/to/'Blackmagic DeckLink SDK'")
    set(WEBLINKED_WITH_DECKLINK OFF)
  endif()
endif()

if(WEBLINKED_WITH_AJA)
  find_package(NTV2)
  if(NTV2_FOUND)
    list(APPEND WEBLINKED_BACKENDS aja)
  else()
    message(STATUS "libajantv2 not found — AJA backend disabled. "
                   "Pass -DNTV2_DIR=/path/to/libajantv2")
    set(WEBLINKED_WITH_AJA OFF)
  endif()
endif()

if(WEBLINKED_WITH_SCREEN)
  list(APPEND WEBLINKED_BACKENDS screen)
endif()

if(WEBLINKED_WITH_SHARED)
  list(APPEND WEBLINKED_BACKENDS shared)
endif()

message(STATUS "WebLinked output backends: preview ${WEBLINKED_BACKENDS}")

# ----------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------

set(WEBLINKED_CORE_SOURCES
  src/core/video_format.cpp
  src/core/video_format.h
  src/core/frame.cpp
  src/core/frame.h
  src/core/frame_ring.h
  src/core/audio_fifo.cpp
  src/core/audio_fifo.h
  src/core/frame_clock.cpp
  src/core/frame_clock.h
  src/core/pixel_convert.cpp
  src/core/pixel_convert.h
  src/core/json.cpp
  src/core/json.h
  src/core/dylib.cpp
  src/core/dylib.h
  src/core/source_config.cpp
  src/core/source_config.h
  src/core/settings_store.cpp
  src/core/settings_store.h
  # The OSC receiver is here rather than with the rest of the control surface
  # because it needs no CEF — only sockets — and its packet decoder is pure
  # parsing that the tests must be able to reach. It was in the engine target
  # until a string-padding bug in that decoder silently dropped a quarter of all
  # messages for a whole release, undetected precisely because nothing that
  # cheap to test was linkable by a test.
  src/control/osc_server.cpp
  src/control/osc_server.h
  src/diag/diag.cpp
  src/diag/diag.h
)

set(WEBLINKED_ENGINE_SOURCES
  src/browser/browser_source.cpp
  src/browser/browser_source.h
  src/browser/render_client.cpp
  src/browser/render_client.h
  src/browser/cef_app.cpp
  src/browser/cef_app.h
  src/outputs/output.cpp
  src/outputs/output.h
  src/outputs/preview_output.cpp
  src/engine/engine.cpp
  src/engine/engine.h
  src/engine/source_manager.cpp
  src/engine/source_manager.h
  src/control/http_server.cpp
  src/control/http_server.h
  src/control/control_api.cpp
  src/control/control_api.h
  src/control/web_assets.h
)

if(WEBLINKED_WITH_NDI)
  list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/ndi_output.cpp)
endif()
if(WEBLINKED_WITH_OMT)
  list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/omt_output.cpp)
endif()
if(WEBLINKED_WITH_DECKLINK)
  list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/decklink_output.cpp)
endif()
if(WEBLINKED_WITH_AJA)
  list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/aja_output.cpp)
endif()
if(WEBLINKED_WITH_SCREEN)
  list(APPEND WEBLINKED_ENGINE_SOURCES
    src/outputs/screen_output.cpp
    src/outputs/screen_output.h
    src/outputs/screen_frame_ring.h
    src/outputs/screen_window.h)
  if(APPLE)
    # A .mm, and deliberately not OBJCXX — see the note at the head of this
    # file. CMake compiles it as CXX and clang infers Objective-C++ from the
    # extension, exactly as it already does for src/app/mac_application.mm.
    list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/screen_window_mac.mm)
  elseif(WIN32)
    list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/screen_window_win.cpp)
  else()
    list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/screen_window_linux.cpp)
  endif()
endif()
if(WEBLINKED_WITH_SHARED)
  list(APPEND WEBLINKED_ENGINE_SOURCES
    src/outputs/shared_output.cpp
    src/outputs/shared_output.h
    src/outputs/shared_surface.h)
  if(APPLE)
    # The same .mm-as-CXX arrangement as the screen output above, plus the
    # vendored Syphon server sources. Those are ARC and expect Syphon's prefix
    # header for SYPHONLOG, neither of which is true of anything else here, so
    # both are set per-file rather than on the target.
    list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/shared_surface_mac.mm)
    set(WEBLINKED_SYPHON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon/Syphon)
    file(GLOB WEBLINKED_SYPHON_OBJC ${WEBLINKED_SYPHON_DIR}/*.m)
    file(GLOB WEBLINKED_SYPHON_C    ${WEBLINKED_SYPHON_DIR}/*.c)
    list(APPEND WEBLINKED_ENGINE_SOURCES
      ${WEBLINKED_SYPHON_OBJC} ${WEBLINKED_SYPHON_C})
    # -w because this is upstream code we do not modify, and it uses
    # kIOSurfaceIsGlobal — which is how Syphon shares a surface between
    # processes at all, and is deprecated with no replacement. Left as a
    # warning it would be noise in every build forever.
    #
    # The prefix header is where Syphon defines SYPHONLOG; its .m files do not
    # compile without it. ARC is Syphon's own setting and applies only to the
    # Objective-C files — SyphonDispatch.c is plain C, where the flag is not
    # merely unnecessary but rejected.
    set_source_files_properties(${WEBLINKED_SYPHON_OBJC} PROPERTIES
      COMPILE_OPTIONS
        "-fobjc-arc;-include;${WEBLINKED_SYPHON_DIR}/Syphon_Prefix.pch;-w")
    set_source_files_properties(${WEBLINKED_SYPHON_C} PROPERTIES
      COMPILE_OPTIONS "-w")
  elseif(WIN32)
    list(APPEND WEBLINKED_ENGINE_SOURCES src/outputs/shared_surface_win.cpp)
    set(WEBLINKED_SPOUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout/Spout)
    file(GLOB WEBLINKED_SPOUT_SOURCES ${WEBLINKED_SPOUT_DIR}/*.cpp)
    list(APPEND WEBLINKED_ENGINE_SOURCES ${WEBLINKED_SPOUT_SOURCES})
    # /w because this is upstream code we do not modify. Spout builds cleanly
    # at its own warning level, not at the one CEF imposes here.
    #
    # The force-includes are for the same reason they are not an edit to the
    # vendored file: SpoutFrameCount.cpp calls timeBeginPeriod/timeGetDevCaps and
    # gets their declarations from <windows.h>, which only pulls in <mmsystem.h>
    # when WIN32_LEAN_AND_MEAN is absent. This build defines it globally (see the
    # NOMINMAX note above), so upstream code that compiles everywhere else fails
    # here with 'TIMECAPS': undeclared identifier.
    #
    # Both headers, in this order, and the order is the whole point: /FI injects
    # at the very top of the translation unit, so naming mmsystem.h alone puts it
    # *before* windows.h and it fails inside mmsyscom.h instead — WINAPI and
    # MMVERSION undeclared, which reads like a broken SDK rather than a missing
    # prerequisite. windows.h first gives it the base types it assumes.
    set_source_files_properties(${WEBLINKED_SPOUT_SOURCES} PROPERTIES
      COMPILE_OPTIONS "/w;/FIwindows.h;/FImmsystem.h")
  endif()
endif()

# ----------------------------------------------------------------------------
# weblinked_core — everything that does not need CEF
#
# Split out so the unit tests build and run in a couple of seconds without
# linking a 200 MB Chromium. Everything genuinely testable lives here.
# ----------------------------------------------------------------------------

add_library(weblinked_core STATIC ${WEBLINKED_CORE_SOURCES})

target_include_directories(weblinked_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(weblinked_core PUBLIC
  WEBLINKED_VERSION="${WEBLINKED_VERSION}"
)

set_target_properties(weblinked_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

if(WIN32)
  # ws2_32 for the sockets; dbghelp for SymInitialize/SymFromAddr, which is how
  # diag.cpp turns a crash backtrace into names rather than bare addresses.
  target_link_libraries(weblinked_core PUBLIC ws2_32 dbghelp)
endif()
if(UNIX)
  target_link_libraries(weblinked_core PUBLIC ${CMAKE_DL_LIBS})
  find_package(Threads REQUIRED)
  target_link_libraries(weblinked_core PUBLIC Threads::Threads)
endif()

# ----------------------------------------------------------------------------
# weblinked_engine — the browser, the outputs and the control surface
# ----------------------------------------------------------------------------

add_library(weblinked_engine STATIC ${WEBLINKED_ENGINE_SOURCES})

target_include_directories(weblinked_engine PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CEF_ROOT}"
)

target_compile_definitions(weblinked_engine PUBLIC
  $<$<BOOL:${WEBLINKED_WITH_NDI}>:WEBLINKED_WITH_NDI=1>
  $<$<BOOL:${WEBLINKED_WITH_OMT}>:WEBLINKED_WITH_OMT=1>
  $<$<BOOL:${WEBLINKED_WITH_DECKLINK}>:WEBLINKED_WITH_DECKLINK=1>
  $<$<BOOL:${WEBLINKED_WITH_AJA}>:WEBLINKED_WITH_AJA=1>
  $<$<BOOL:${WEBLINKED_WITH_SCREEN}>:WEBLINKED_WITH_SCREEN=1>
  $<$<BOOL:${WEBLINKED_WITH_SHARED}>:WEBLINKED_WITH_SHARED=1>
)

if(WEBLINKED_WITH_SHARED)
  if(APPLE)
    # third_party/syphon is the include root, not .../syphon/Syphon: the
    # vendored sources import <Syphon/SyphonServerBase.h> framework-style, and
    # that layout is what makes those resolve without editing upstream files.
    target_include_directories(weblinked_engine PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon)
    # IOSurface is the shared buffer itself. No Metal and no OpenGL: this
    # backend writes the surface with the CPU, which is where CEF's pixels
    # already are.
    target_link_libraries(weblinked_engine PUBLIC
      "-framework IOSurface" "-framework Foundation")
  elseif(WIN32)
    # Flat include root: the vendored Spout files sit in one directory, which
    # upstream supports explicitly — SpoutDX.h picks its include prefix with
    # __has_include("SpoutCommon.h"), so no file needed editing.
    target_include_directories(weblinked_engine PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spout/Spout)
    # d3d11 and dxgi only. spoutDX is the DirectX half of the SDK and pulls in
    # no OpenGL; psapi arrives through a #pragma comment(lib) in SpoutDX.h.
    # winmm carries the multimedia timer SpoutFrameCount uses — declaring it via
    # /FImmsystem.h above only fixes the compile; the symbols still have to come
    # from somewhere at link time.
    target_link_libraries(weblinked_engine PUBLIC d3d11 dxgi winmm)
  endif()
endif()

if(WEBLINKED_WITH_SCREEN)
  if(APPLE)
    # QuartzCore carries CAMetalLayer; CoreVideo carries CVDisplayLink. Cocoa
    # and Metal are the obvious two.
    target_link_libraries(weblinked_engine PUBLIC
      "-framework Metal" "-framework QuartzCore" "-framework CoreVideo")
  elseif(WIN32)
    # d3dcompiler because the shaders are compiled from source at open() rather
    # than built into bytecode — no fxc step, no generated headers.
    target_link_libraries(weblinked_engine PUBLIC d3d11 dxgi d3dcompiler)
  else()
    target_link_libraries(weblinked_engine PUBLIC
      ${X11_LIBRARIES} ${WEBLINKED_EGL_LIBRARY} ${WEBLINKED_GLES_LIBRARY})
    target_include_directories(weblinked_engine PRIVATE ${X11_INCLUDE_DIR})
  endif()
endif()

set_target_properties(weblinked_engine PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(weblinked_engine PUBLIC
  weblinked_core libcef_dll_wrapper ${CEF_STANDARD_LIBS})

# CEF's flag lists must be applied verbatim: a mismatch against a prebuilt
# Chromium surfaces as a link error, or as a crash inside CefInitialize.
#
# The C++ list is scoped to C++ because it genuinely is C++-only — it carries
# -std=c++20, which clang rejects outright on an Objective-C translation unit.
# That was harmless while every file here was .cpp or .mm (both compiled as
# CXX; see the note at the head of this file), and stopped being harmless when
# the vendored Syphon .m files arrived. CEF_COMPILER_FLAGS stays unscoped: it
# is the language-neutral half, and CEF's ABI depends on it applying
# everywhere.
target_compile_options(weblinked_engine PRIVATE
  ${CEF_COMPILER_FLAGS}
  $<$<COMPILE_LANGUAGE:CXX>:${CEF_CXX_COMPILER_FLAGS}>)

if(WEBLINKED_WITH_NDI)
  target_include_directories(weblinked_engine PRIVATE ${NDI_INCLUDE_DIRS})
  target_compile_definitions(weblinked_engine PRIVATE NDI_LIBRARY_NAME="${NDI_LIBRARY_NAME}")
endif()
if(WEBLINKED_WITH_OMT)
  target_include_directories(weblinked_engine PRIVATE ${OMT_INCLUDE_DIRS})
  target_compile_definitions(weblinked_engine PRIVATE OMT_LIBRARY_NAME="${OMT_LIBRARY_NAME}")
endif()
if(WEBLINKED_WITH_DECKLINK)
  target_include_directories(weblinked_engine PRIVATE ${DeckLinkSDK_INCLUDE_DIRS})
  target_sources(weblinked_engine PRIVATE ${DeckLinkSDK_DISPATCH_SOURCES})
  if(DeckLinkSDK_LIBRARIES)
    target_link_libraries(weblinked_engine PUBLIC ${DeckLinkSDK_LIBRARIES})
  endif()
endif()
if(WEBLINKED_WITH_AJA)
  target_link_libraries(weblinked_engine PUBLIC ${NTV2_LIBRARIES})
  target_include_directories(weblinked_engine PRIVATE ${NTV2_INCLUDE_DIRS})
endif()

# ----------------------------------------------------------------------------
# Application
# ----------------------------------------------------------------------------

if(OS_MAC)
  include(MacBundle)
  weblinked_add_mac_app()
else()
  # Console subsystem, deliberately, rather than add_executable(... WIN32 ...).
  # WIN32 makes MSVC demand WinMain, and the fix people reach for — /ENTRY or a
  # WinMain shim — buys a windowless process at the cost of --help, the version
  # banner and the startup errors having nowhere to go. WebLinked is driven from
  # a command line often enough that losing its output is the worse trade; the
  # tray launcher covers the case where nobody wants a terminal.
  add_executable(weblinked src/app/main.cpp)
  set_target_properties(weblinked PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_link_libraries(weblinked PRIVATE weblinked_engine libcef_lib)

  if(MSVC AND WEBLINKED_WITH_OMT)
    # libomt.h carries #pragma comment(lib, "libomt.lib"), so merely including it
    # makes MSVC auto-link an import library we deliberately do not have: every
    # OMT entry point is resolved with GetProcAddress at run time, which is what
    # lets the binary start on a machine with no OMT installed and simply report
    # the output unavailable. Without this the link dies with
    # "LNK1104: cannot open file 'libomt.lib'".
    target_link_options(weblinked PRIVATE "/NODEFAULTLIB:libomt.lib")
  endif()
  target_compile_options(weblinked PRIVATE ${CEF_COMPILER_FLAGS} ${CEF_CXX_COMPILER_FLAGS})
  target_link_options(weblinked PRIVATE ${CEF_LINKER_FLAGS} ${CEF_EXE_LINKER_FLAGS})

  SET_CEF_TARGET_OUT_DIR()
  COPY_FILES(weblinked "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
  COPY_FILES(weblinked "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
endif()

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------

if(WEBLINKED_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
