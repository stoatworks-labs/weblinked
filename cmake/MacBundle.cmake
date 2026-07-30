# The macOS application bundle.
#
# A CEF app on macOS is not one executable, it is six: the browser process plus
# five helper .app bundles (default, Alerts, GPU, Plugin, Renderer) which CEF
# locates *by name* inside Contents/Frameworks. Get a name wrong and the app
# starts, shows a window and then dies the moment it needs a renderer.
#
# Two traps are worth naming, because both cost real time:
#
# 1. The helper bundle names must be "<app name> Helper[ (variant)].app". CEF
#    derives them from the main bundle; browser_subprocess_path is deliberately
#    NOT set on macOS.
#
# 2. Gatekeeper. An unsigned .app that bundles further executables inside
#    itself does not become trusted when the user approves the outer app — the
#    nested helpers get SIGKILLed with no dialog and no log entry, which reads
#    exactly like a CEF initialisation bug. Every build is therefore ad-hoc
#    signed inside-out below. For distribution, replace the ad-hoc identity
#    with a Developer ID and notarise.

function(weblinked_add_mac_app)
  set(_app_name "WebLinked")

  SET_CEF_TARGET_OUT_DIR()
  set(_out_dir "${CEF_TARGET_OUT_DIR}")
  set(_app_bundle "${_out_dir}/${_app_name}.app")

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY)

  # ---- helper processes -----------------------------------------------------

  set(_helper_targets "")

  foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
    string(REPLACE ":" ";" _suffixes "${_suffixes}")
    list(GET _suffixes 0 _name_suffix)
    list(GET _suffixes 1 _target_suffix)
    list(GET _suffixes 2 _plist_suffix)

    set(_target "weblinked_helper${_target_suffix}")
    set(_helper_name "${_app_name} Helper${_name_suffix}")

    set(WEBLINKED_HELPER_NAME "${_helper_name}")
    set(WEBLINKED_HELPER_BUNDLE_ID "works.stoat.weblinked.helper${_plist_suffix}")
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/src/app/helper-Info.plist.in"
      "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_plist_suffix}.plist"
      @ONLY)

    add_executable(${_target} MACOSX_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/helper_main.cpp")
    set_target_properties(${_target} PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      OUTPUT_NAME "${_helper_name}"
      RUNTIME_OUTPUT_DIRECTORY "${_out_dir}"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_plist_suffix}.plist"
    )
    target_include_directories(${_target} PRIVATE "${CEF_ROOT}")
    target_link_libraries(${_target} PRIVATE libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_compile_options(${_target} PRIVATE ${CEF_COMPILER_FLAGS} ${CEF_CXX_COMPILER_FLAGS})
    target_link_options(${_target} PRIVATE ${CEF_LINKER_FLAGS} ${CEF_EXE_LINKER_FLAGS})

    list(APPEND _helper_targets ${_target})
  endforeach()

  # ---- browser process -----------------------------------------------------

  add_executable(weblinked MACOSX_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
  set_target_properties(weblinked PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "${_app_name}"
    RUNTIME_OUTPUT_DIRECTORY "${_out_dir}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  )
  target_link_libraries(weblinked PRIVATE weblinked_engine)
  target_compile_options(weblinked PRIVATE ${CEF_COMPILER_FLAGS} ${CEF_CXX_COMPILER_FLAGS})
  target_link_options(weblinked PRIVATE ${CEF_LINKER_FLAGS} ${CEF_EXE_LINKER_FLAGS})

  add_dependencies(weblinked ${_helper_targets})

  # The CEF framework carries the Chromium binary and all its resources.
  COPY_MAC_FRAMEWORK(weblinked "${CEF_BINARY_DIR}" "${_app_bundle}")

  # Helpers live inside the outer bundle.
  foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
    string(REPLACE ":" ";" _suffixes "${_suffixes}")
    list(GET _suffixes 0 _name_suffix)
    set(_helper_name "${_app_name} Helper${_name_suffix}")
    add_custom_command(TARGET weblinked POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${_out_dir}/${_helper_name}.app"
              "${_app_bundle}/Contents/Frameworks/${_helper_name}.app"
      VERBATIM)
  endforeach()

  # Ad-hoc sign inside-out. See the Gatekeeper note at the top of this file.
  add_custom_command(TARGET weblinked POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DAPP_BUNDLE=${_app_bundle}
            -DAPP_NAME=${_app_name}
            -DWEBLINKED_CODESIGN_IDENTITY=${WEBLINKED_CODESIGN_IDENTITY}
            -DENTITLEMENTS=${CMAKE_CURRENT_SOURCE_DIR}/src/app/WebLinked.entitlements
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SignMacBundle.cmake"
    COMMENT "Signing ${_app_name}.app"
    VERBATIM)

  set(WEBLINKED_MAC_APP "${_app_bundle}" PARENT_SCOPE)
endfunction()
