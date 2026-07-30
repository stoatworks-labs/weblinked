# Finds the Blackmagic DeckLink SDK headers.
#
# The SDK is a free but licence-gated download from Blackmagic and is not ours
# to redistribute, so nothing from it is vendored here. Point the build at your
# own copy:
#
#   -DDECKLINK_SDK_DIR="/path/to/Blackmagic DeckLink SDK 12.9"
#
# Accepted layouts:
#   <dir>/Mac/include/DeckLinkAPI.h     (official SDK archive)
#   <dir>/Linux/include/DeckLinkAPI.h
#   <dir>/Win/include/DeckLinkAPI.h
#   <dir>/DeckLinkAPI.h                 (a bare include directory)
#
# On macOS and Linux the API is reached through DeckLinkAPIDispatch.cpp, which
# ships in the SDK and dlopens the installed DeckLinkAPI framework/library. That
# is why there is no link library on those platforms: an executable built with
# the SDK still runs on a machine with no Desktop Video installed, and simply
# finds no devices.
#
# Sets: DeckLinkSDK_FOUND, DeckLinkSDK_INCLUDE_DIRS,
#       DeckLinkSDK_DISPATCH_SOURCES, DeckLinkSDK_LIBRARIES

set(_dl_hints
  "${DECKLINK_SDK_DIR}"
  "$ENV{DECKLINK_SDK_DIR}"
)

find_path(DeckLinkSDK_INCLUDE_DIR
  NAMES DeckLinkAPI.h
  HINTS ${_dl_hints}
  PATH_SUFFIXES
    Mac/include Linux/include Win/include include
    "Blackmagic DeckLink SDK/Mac/include"
    # Some redistributions flatten the layout to <dir>/<Platform>/ with the
    # headers directly inside — Unreal's BlackmagicMedia third-party copy, for
    # one, which is a convenient way to build the backend without the SDK
    # archive to hand.
    Mac Linux Win
  NO_DEFAULT_PATH
  DOC "Directory containing DeckLinkAPI.h"
)

set(DeckLinkSDK_DISPATCH_SOURCES "")
set(DeckLinkSDK_LIBRARIES "")

if(DeckLinkSDK_INCLUDE_DIR)
  if(NOT WIN32)
    # Required on macOS and Linux: it provides CreateDeckLinkIteratorInstance()
    # and friends by dlopening the driver's library.
    if(EXISTS "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
      set(DeckLinkSDK_DISPATCH_SOURCES "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
    else()
      message(WARNING "DeckLink SDK: DeckLinkAPIDispatch.cpp missing from "
                      "${DeckLinkSDK_INCLUDE_DIR} — link errors are likely.")
    endif()
  endif()

  if(APPLE)
    find_library(_dl_corefoundation CoreFoundation REQUIRED)
    list(APPEND DeckLinkSDK_LIBRARIES ${_dl_corefoundation})
  elseif(UNIX)
    list(APPEND DeckLinkSDK_LIBRARIES ${CMAKE_DL_LIBS})
  endif()

  # Report the SDK version so a build log records what it was compiled against.
  if(EXISTS "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPIVersion.h")
    file(STRINGS "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPIVersion.h" _ver
         REGEX "BLACKMAGIC_DECKLINK_API_VERSION_STRING")
    string(REGEX MATCH "\"([0-9.]+)\"" _m "${_ver}")
    set(DeckLinkSDK_VERSION "${CMAKE_MATCH_1}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeckLinkSDK
  REQUIRED_VARS DeckLinkSDK_INCLUDE_DIR
  VERSION_VAR DeckLinkSDK_VERSION
  FAIL_MESSAGE "DeckLink SDK not found. Pass -DDECKLINK_SDK_DIR=<sdk root>."
)

if(DeckLinkSDK_FOUND)
  set(DeckLinkSDK_INCLUDE_DIRS "${DeckLinkSDK_INCLUDE_DIR}")
endif()

mark_as_advanced(DeckLinkSDK_INCLUDE_DIR)
