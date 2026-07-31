# Unit tests for everything that does not need a browser or a video card.
#
# Deliberately linked against weblinked_core only, not weblinked_engine: these
# must stay quick enough to run on every edit.

add_executable(weblinked_tests
  main.cpp
  test_video_format.cpp
  test_pixel_convert.cpp
  test_audio_fifo.cpp
  test_frame_clock.cpp
  test_json.cpp
  test_source_config.cpp
  test_settings_store.cpp
  test_osc_server.cpp
  test_screen_geometry.cpp
)

set_target_properties(weblinked_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(weblinked_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_link_libraries(weblinked_tests PRIVATE weblinked_core)

add_test(NAME weblinked_tests COMMAND weblinked_tests)
