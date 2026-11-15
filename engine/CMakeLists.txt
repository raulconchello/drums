# drums-engine: real-time audio engine (JACK/PipeWire + sfizz) of the drum practice app.
#
#   mise exec -- cmake -S engine -B engine/build -G Ninja
#   mise exec -- cmake --build engine/build          # -> engine/build/drums-engine
#
# Options:
#   DRUMS_WITH_CORE     link engine/core (scoring, recording, WebSocket). Default: ON when
#                       engine/core/CMakeLists.txt exists. OFF builds an audio-only engine.
#   DRUMS_ENGINE_TESTS  build the engine tests (engine/tests).
cmake_minimum_required(VERSION 3.28)
project(drums_engine LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_drums_core_present OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt)
    set(_drums_core_present ON)
endif()
option(DRUMS_WITH_CORE "Link the drums_core library (scoring, recording, WebSocket server)" ${_drums_core_present})
option(DRUMS_ENGINE_TESTS "Build the engine tests" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
find_package(Threads REQUIRED)

# Third-party code (sfizz + its abseil) is built as C++17: sfizz defaults to C++14, where
# absl::string_view is not std::string_view, which would break the sfizz parser API we call from
# C++20 code. Our own targets request C++20 through target_compile_features.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard for third-party code")

include(FetchContent)
# Same pin as engine/core (the first declaration of a name wins, so both stay in sync).
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

# ---- sfizz (BSD-2-Clause), pinned release, static, no clients/plugins/demos ----
set(SFIZZ_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfizz)
if(NOT EXISTS ${SFIZZ_SOURCE_DIR}/CMakeLists.txt)
    message(STATUS "sfizz sources missing: running third_party/fetch-sfizz.sh")
    execute_process(COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fetch-sfizz.sh RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "fetching sfizz failed (${_rc})")
    endif()
endif()
block()
    # CMake 4 refuses cmake_minimum_required(VERSION < 3.5) in sfizz's bundled libaiff/st_audiofile.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(SFIZZ_JACK OFF)
    set(SFIZZ_RENDER OFF)
    set(SFIZZ_SHARED OFF)
    set(SFIZZ_TESTS OFF)
    set(SFIZZ_DEMOS OFF)
    set(SFIZZ_DEVTOOLS OFF)
    set(SFIZZ_BENCHMARKS OFF)
    set(SFIZZ_USE_SNDFILE OFF)
    set(ENABLE_LTO OFF)
    set(BUILD_TESTING OFF)
    add_subdirectory(${SFIZZ_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sfizz EXCLUDE_FROM_ALL SYSTEM)
endblock()

# ---- core library (implemented separately in engine/core) ----
if(DRUMS_WITH_CORE)
    if(NOT _drums_core_present)
        message(FATAL_ERROR "DRUMS_WITH_CORE=ON but engine/core/CMakeLists.txt does not exist")
    endif()
    add_subdirectory(core)
endif()

# ---- audio library: everything testable without JACK and without core ----
add_library(drums_audio STATIC
    src/audio_engine.cpp
    src/click.cpp
    src/drum_sampler.cpp
    src/kit.cpp
    src/options.cpp
    src/session.cpp
    src/sfz_router.cpp
    src/wav_writer.cpp)
target_include_directories(drums_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include)   # contract headers (types.h, exercise.h: header-only parts)
target_compile_features(drums_audio PUBLIC cxx_std_20)
target_link_libraries(drums_audio
    PUBLIC sfizz::sfizz Threads::Threads
    PRIVATE sfizz::parser nlohmann_json::nlohmann_json)
target_compile_options(drums_audio PRIVATE -Wall -Wextra)

# ---- executable ----
add_executable(drums-engine src/main.cpp src/jack_backend.cpp)
if(DRUMS_WITH_CORE)
    target_sources(drums-engine PRIVATE src/app_core.cpp)
    target_link_libraries(drums-engine PRIVATE drums_core)
    target_compile_definitions(drums-engine PRIVATE DRUMS_WITH_CORE=1)
else()
    target_sources(drums-engine PRIVATE src/app_basic.cpp)
endif()
target_link_libraries(drums-engine PRIVATE drums_audio PkgConfig::JACK)
target_compile_features(drums-engine PRIVATE cxx_std_20)
target_compile_options(drums-engine PRIVATE -Wall -Wextra)
set_target_properties(drums-engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "drums-engine: core library ${DRUMS_WITH_CORE}, tests ${DRUMS_ENGINE_TESTS}")

if(DRUMS_ENGINE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
