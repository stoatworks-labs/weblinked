# Codesigns a CEF app bundle from the inside out.
#
# Run as a script (cmake -P) from a POST_BUILD step.
#
# Two things here are load-bearing and were both learned the hard way:
#
# 1. Order. A nested bundle signed *after* its container invalidates the
#    container's signature, so this walks inwards-out: framework libraries,
#    framework, helpers, then the app.
#
# 2. Hardened runtime and ad-hoc signatures do not mix. With `--options runtime`
#    macOS enforces library validation, which requires every loaded library to
#    carry the same Team ID as the process. An ad-hoc signature has no Team ID at
#    all, so the app fails at launch with
#
#      "code signature ... not valid for use in process: mapping process and
#       mapped file (non-platform) have different Team IDs"
#
#    which reads like a broken bundle rather than a signing policy. So: ad-hoc
#    builds are signed *without* hardened runtime. A real distribution build
#    passes -DWEBLINKED_CODESIGN_IDENTITY="Developer ID Application: ..." and
#    then gets hardened runtime plus the library-validation exemption that every
#    Chromium-based app needs.

if(NOT APP_BUNDLE OR NOT APP_NAME)
  message(FATAL_ERROR "APP_BUNDLE and APP_NAME are required")
endif()

find_program(CODESIGN codesign)
if(NOT CODESIGN)
  message(WARNING "codesign not found; leaving ${APP_NAME}.app unsigned. "
                  "Nested helper processes will be killed on launch.")
  return()
endif()

if(WEBLINKED_CODESIGN_IDENTITY)
  set(_identity "${WEBLINKED_CODESIGN_IDENTITY}")
  set(_hardened TRUE)
else()
  set(_identity "-")   # ad-hoc
  set(_hardened FALSE)
endif()

function(_sign path)
  set(_args --force --sign "${_identity}" --timestamp=none)
  if(_hardened)
    list(APPEND _args --options runtime)
    if(ENTITLEMENTS AND EXISTS "${ENTITLEMENTS}")
      list(APPEND _args --entitlements "${ENTITLEMENTS}")
    endif()
  endif()
  execute_process(
    COMMAND "${CODESIGN}" ${_args} "${path}"
    RESULT_VARIABLE _res
    OUTPUT_QUIET
    ERROR_VARIABLE _err)
  if(NOT _res EQUAL 0)
    message(FATAL_ERROR "codesign failed for ${path}:\n${_err}")
  endif()
endfunction()

# 1. The framework, and every library it carries.
set(_framework "${APP_BUNDLE}/Contents/Frameworks/Chromium Embedded Framework.framework")
if(EXISTS "${_framework}")
  file(GLOB _libs "${_framework}/Versions/A/Libraries/*.dylib")
  foreach(_lib ${_libs})
    _sign("${_lib}")
  endforeach()
  _sign("${_framework}/Versions/A")
endif()

# 2. Each helper app.
file(GLOB _helpers "${APP_BUNDLE}/Contents/Frameworks/${APP_NAME} Helper*.app")
foreach(_helper ${_helpers})
  _sign("${_helper}")
endforeach()

# 3. The outer bundle last.
_sign("${APP_BUNDLE}")
