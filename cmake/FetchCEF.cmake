# Locates a CEF binary distribution, downloading one if we have to.
#
# Resolution order:
#   1. -DCEF_ROOT=<dir>                       (an unpacked distribution)
#   2. third_party/cef/current                (what a previous run downloaded)
#   3. download WEBLINKED_CEF_VERSION from the Spotify CDN and unpack it there
#
# The version is pinned rather than resolved from index.json at configure time,
# because a build that silently follows upstream stable is a build that breaks
# on someone else's Tuesday.

set(WEBLINKED_CEF_VERSION "150.0.17+g94c1726+chromium-150.0.7871.187"
    CACHE STRING "CEF binary distribution version")

# sha1 of the *_minimal.tar.bz2 archive for each supported platform, as
# published in https://cef-builds.spotifycdn.com/index.json.
set(_weblinked_cef_sha1_macosarm64   "ade718b28418a975efb98bb4b0c9daab52c12dc0")

function(_weblinked_cef_platform out_var)
  if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64")
      set(${out_var} "macosarm64" PARENT_SCOPE)
    else()
      set(${out_var} "macosx64" PARENT_SCOPE)
    endif()
  elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
      set(${out_var} "windowsarm64" PARENT_SCOPE)
    else()
      set(${out_var} "windows64" PARENT_SCOPE)
    endif()
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(${out_var} "linuxarm64" PARENT_SCOPE)
    else()
      set(${out_var} "linux64" PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(weblinked_provision_cef)
  if(CEF_ROOT AND IS_DIRECTORY "${CEF_ROOT}/cmake")
    message(STATUS "CEF: using ${CEF_ROOT}")
    return()
  endif()

  set(_cache "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
  if(IS_DIRECTORY "${_cache}/current/cmake")
    set(CEF_ROOT "${_cache}/current" CACHE PATH "" FORCE)
    message(STATUS "CEF: using cached ${CEF_ROOT}")
    return()
  endif()

  _weblinked_cef_platform(_plat)
  set(_name "cef_binary_${WEBLINKED_CEF_VERSION}_${_plat}_minimal")
  # '+' is legal in a path but must be percent-encoded in the URL.
  string(REPLACE "+" "%2B" _url_version "${WEBLINKED_CEF_VERSION}")
  set(_url "https://cef-builds.spotifycdn.com/cef_binary_${_url_version}_${_plat}_minimal.tar.bz2")

  set(_expected "${_weblinked_cef_sha1_${_plat}}")

  message(STATUS "CEF: downloading ${_url}")
  file(MAKE_DIRECTORY "${_cache}")
  set(_archive "${_cache}/${_name}.tar.bz2")

  if(_expected)
    file(DOWNLOAD "${_url}" "${_archive}" SHOW_PROGRESS
         EXPECTED_HASH SHA1=${_expected} STATUS _status)
  else()
    # No pinned hash for this platform yet; still fail loudly on a bad download.
    message(WARNING "CEF: no pinned sha1 for ${_plat}; archive integrity unverified")
    file(DOWNLOAD "${_url}" "${_archive}" SHOW_PROGRESS STATUS _status)
  endif()

  list(GET _status 0 _code)
  if(NOT _code EQUAL 0)
    list(GET _status 1 _msg)
    message(FATAL_ERROR "CEF: download failed (${_msg}).\n"
                        "Unpack a distribution yourself and pass -DCEF_ROOT=<dir>.")
  endif()

  message(STATUS "CEF: unpacking (this takes a minute)")
  file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_cache}")
  file(RENAME "${_cache}/${_name}" "${_cache}/current")
  file(REMOVE "${_archive}")

  set(CEF_ROOT "${_cache}/current" CACHE PATH "" FORCE)
  message(STATUS "CEF: provisioned at ${CEF_ROOT}")
endfunction()
