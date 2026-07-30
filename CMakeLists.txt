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

project(weblinked VERSION 0.1.0 LANGUAGES C CXX)

set(WEBLINKED_VERSION "${PROJECT_VERSION}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------

option(WEBLINKED_WITH_NDI      "Build the NDI output backend"      ON)
option(WEBLINKED_WITH_OMT      "Build the OMT output backend"      ON)
option(WEBLINKED_WITH_DECKLINK "Build the DeckLink output backend" ON)
option(WEBLINKED_WITH_AJA      "Build the AJA NTV2 output backend" OFF)
option(WEBLINKED_BUILD_TESTS   "Build the unit tests"              ON)

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
  src/control/http_server.cpp
  src/control/http_server.h
  src/control/control_api.cpp
  src/control/control_api.h
  src/control/osc_server.cpp
  src/control/osc_server.h
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
  target_link_libraries(weblinked_core PUBLIC ws2_32)
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
)

set_target_properties(weblinked_engine PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(weblinked_engine PUBLIC
  weblinked_core libcef_dll_wrapper ${CEF_STANDARD_LIBS})

# CEF's flag lists must be applied verbatim: a mismatch against a prebuilt
# Chromium surfaces as a link error, or as a crash inside CefInitialize.
target_compile_options(weblinked_engine PRIVATE ${CEF_COMPILER_FLAGS} ${CEF_CXX_COMPILER_FLAGS})

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
  add_executable(weblinked WIN32 src/app/main.cpp)
  set_target_properties(weblinked PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_link_libraries(weblinked PRIVATE weblinked_engine)
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
