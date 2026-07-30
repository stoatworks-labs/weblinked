# Finds the NDI SDK headers.
#
# We deliberately do *not* find the library: libndi is loaded with dlopen at
# run time (see src/outputs/ndi_output.cpp). NDI's licence does not let us
# redistribute it, and a build machine with the SDK should still produce a
# binary that runs on a machine with only the NDI runtime redistributable —
# or with neither, in which case the NDI output simply reports unavailable.
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

find_path(NDI_INCLUDE_DIR
  NAMES Processing.NDI.Lib.h
  HINTS ${_ndi_hints}
  PATH_SUFFIXES include Include
  DOC "Directory containing Processing.NDI.Lib.h"
)

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
  FAIL_MESSAGE "NDI SDK headers not found. Pass -DNDI_SDK_DIR=<sdk root>."
)

if(NDI_FOUND)
  set(NDI_INCLUDE_DIRS "${NDI_INCLUDE_DIR}")
endif()

mark_as_advanced(NDI_INCLUDE_DIR)
