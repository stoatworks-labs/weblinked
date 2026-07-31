# Finds the NDI SDK headers.
#
# We deliberately do *not* find the library: libndi is loaded with dlopen at
# run time (see src/outputs/ndi_output.cpp). The SDK licence permits shipping
# the runtime inside an application, but only under a licence forbidding
# modification and reverse engineering, which MIT cannot impose — so a build
# machine with the SDK still produces a binary that runs on a machine with only
# the NDI redistributable, or with neither, in which case the NDI output simply
# reports itself unavailable. See docs/06-ndi-distribution.md.
#
# The headers themselves are a different matter: each carries its own explicit
# MIT grant ("applies to this file ONLY"), so they are vendored in
# third_party/ndi and used whenever no SDK is installed. Without that fallback
# find_package failed on any machine without the SDK — including every CI
# runner — and the backend was silently dropped from released binaries.
#
# Sets: NDI_FOUND, NDI_INCLUDE_DIRS, NDI_LIBRARY_NAME

set(_ndi_hints
  "${NDI_SDK_DIR}"
  "$ENV{NDI_SDK_DIR}"
  "/Library/NDI SDK for Apple"
  "/Library/NDI Advanced SDK for Apple"
  "C:/Program Files/NDI/NDI 6 SDK"
  "C:/Program Files/NDI/NDI 6 Advanced SDK"
  "/usr/local/ndi"
  "/opt/ndi"
)

# An installed SDK wins, so a developer testing against a newer one gets it.
find_path(NDI_INCLUDE_DIR
  NAMES Processing.NDI.Lib.h
  HINTS ${_ndi_hints}
  PATH_SUFFIXES include Include
  NO_CMAKE_FIND_ROOT_PATH
  DOC "Directory containing Processing.NDI.Lib.h"
)

# Otherwise fall back to the vendored copy, so the backend always builds.
set(_ndi_vendored "${CMAKE_CURRENT_LIST_DIR}/../third_party/ndi/include")
if(NOT NDI_INCLUDE_DIR AND EXISTS "${_ndi_vendored}/Processing.NDI.Lib.h")
  set(NDI_INCLUDE_DIR "${_ndi_vendored}" CACHE PATH
      "Directory containing Processing.NDI.Lib.h" FORCE)
  message(STATUS "NDI SDK not installed — using the vendored headers in third_party/ndi")
endif()

# The soname we dlopen at run time.
if(APPLE)
  set(NDI_LIBRARY_NAME "libndi.dylib")
elseif(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NDI_LIBRARY_NAME "Processing.NDI.Lib.x64.dll")
  else()
    set(NDI_LIBRARY_NAME "Processing.NDI.Lib.x86.dll")
  endif()
else()
  set(NDI_LIBRARY_NAME "libndi.so.6")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI
  REQUIRED_VARS NDI_INCLUDE_DIR
  FAIL_MESSAGE "NDI headers not found, and third_party/ndi is missing. Pass -DNDI_SDK_DIR=<sdk root>."
)

if(NDI_FOUND)
  set(NDI_INCLUDE_DIRS "${NDI_INCLUDE_DIR}")
endif()

mark_as_advanced(NDI_INCLUDE_DIR)
