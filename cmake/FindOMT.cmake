# Finds the Open Media Transport C header (libomt.h).
#
# OMT is MIT licensed, so libomt.h is vendored in third_party/omt and this
# backend builds out of the box. The *library* is still loaded at run time,
# because the prebuilt libomt is published only for Windows (x64, arm64) and
# macOS arm64 — there is no Linux binary release, and a Linux deployment has to
# build libomtnet from source. Loading late keeps that a deployment problem
# rather than a build-time one.
#
# To build against a newer header than the vendored one, get the release from
#   https://github.com/openmediatransport/libomtnet/releases
# and point -DOMT_SDK_DIR at the unpacked Libraries/<platform> directory.
#
# Sets: OMT_FOUND, OMT_INCLUDE_DIRS, OMT_LIBRARY_NAME

set(_omt_hints
  "${OMT_SDK_DIR}"
  "$ENV{OMT_SDK_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/omt"
  "/Library/OMT"
  "/usr/local/include"
  "C:/Program Files/Open Media Transport"
)

find_path(OMT_INCLUDE_DIR
  NAMES libomt.h
  HINTS ${_omt_hints}
  PATH_SUFFIXES include Libraries/MacOS Libraries/Winx64 Libraries/Winarm64
  DOC "Directory containing libomt.h"
)

if(APPLE)
  set(OMT_LIBRARY_NAME "libomt.dylib")
elseif(WIN32)
  set(OMT_LIBRARY_NAME "libomt.dll")
else()
  set(OMT_LIBRARY_NAME "libomt.so")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OMT
  REQUIRED_VARS OMT_INCLUDE_DIR
  FAIL_MESSAGE "libomt.h not found. Pass -DOMT_SDK_DIR=<dir containing libomt.h>."
)

if(OMT_FOUND)
  set(OMT_INCLUDE_DIRS "${OMT_INCLUDE_DIR}")
endif()

mark_as_advanced(OMT_INCLUDE_DIR)
