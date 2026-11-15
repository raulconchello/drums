cmake_path(SET DRUMS_REPO_ROOT NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(drums-engine-tests test_engine.cpp)
target_link_libraries(drums-engine-tests PRIVATE drums_audio)
target_compile_definitions(drums-engine-tests PRIVATE DRUMS_REPO_ROOT="${DRUMS_REPO_ROOT}")
target_compile_options(drums-engine-tests PRIVATE -Wall -Wextra)
add_test(NAME engine COMMAND drums-engine-tests)

# Offline render of click + groove, analysed sample-exactly by analyze_render.py.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME offline-render
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/analyze_render.py
            --engine $<TARGET_FILE:drums-engine> --out ${CMAKE_CURRENT_BINARY_DIR}/render)
endif()

# Live-test tools (see engine/tests/live_test.sh).
pkg_check_modules(ALSA IMPORTED_TARGET alsa)
if(ALSA_FOUND)
    add_executable(midi-sender midi_sender.cpp)
    target_link_libraries(midi-sender PRIVATE PkgConfig::ALSA)
    target_compile_features(midi-sender PRIVATE cxx_std_20)
endif()
add_executable(jack-probe jack_probe.cpp)
target_link_libraries(jack-probe PRIVATE PkgConfig::JACK)
target_compile_features(jack-probe PRIVATE cxx_std_20)
