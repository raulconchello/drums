cmake_minimum_required(VERSION 3.28...3.31)
project(drums_core LANGUAGES CXX)

# Works standalone (cmake -S engine/core -B engine/core/build) and via add_subdirectory(core).
option(DRUMS_CORE_TESTS "Build the drums_core unit tests" ${PROJECT_IS_TOP_LEVEL})

include(FetchContent)

# ---- dependencies (pinned) ----
# The first FetchContent_Declare of a name wins, so a parent project that declares the same
# names first keeps control of the versions.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM)

# IXWebSocket 11.4.5: SocketServer::stop() closes its listening fd without resetting it, and
# stop() runs again from both destructors, so the same fd number is closed several times
# (it may belong to another thread by then). The patch makes the close happen once.
set(_drums_ix_patch "${CMAKE_CURRENT_BINARY_DIR}/drums_patch_ixwebsocket.cmake")
file(WRITE "${_drums_ix_patch}" [=[
set(_file "${SRC}/ixwebsocket/IXSocketServer.cpp")
file(READ "${_file}" _content)
string(FIND "${_content}" "drums-patch" _done)
if(_done EQUAL -1)
    string(REPLACE "Socket::closeSocket(_serverFd);"
        "{ if (_serverFd != -1) Socket::closeSocket(_serverFd); _serverFd = -1; } // drums-patch"
        _content "${_content}")
    file(WRITE "${_file}" "${_content}")
endif()
]=])

set(USE_TLS OFF)            # IXWebSocket options (its option() calls honour normal variables)
set(USE_ZLIB OFF)
set(IXWEBSOCKET_INSTALL OFF)
FetchContent_Declare(ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.5.tar.gz
    URL_HASH SHA256=c5fc225edec32bf7d583e55347ef2c9c4940d005c13ef5e848354a85602f5fd6
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PATCH_COMMAND ${CMAKE_COMMAND} -DSRC=<SOURCE_DIR> -P ${_drums_ix_patch}
    EXCLUDE_FROM_ALL
    SYSTEM)

FetchContent_MakeAvailable(nlohmann_json ixwebsocket)
if(TARGET ixwebsocket AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ixwebsocket PRIVATE -w)  # third-party warnings are not ours to fix
endif()

# ---- library ----
add_library(drums_core STATIC
    src/json_util.h
    src/exercise.cpp
    src/scoring.cpp
    src/midi_recorder.cpp
    src/protocol.cpp
    src/control_server.cpp)
add_library(drums::core ALIAS drums_core)

target_include_directories(drums_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(drums_core PUBLIC cxx_std_20)
set_target_properties(drums_core PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
# PRIVATE: nlohmann and IXWebSocket stay out of the public headers (CMake still links them in).
target_link_libraries(drums_core PRIVATE nlohmann_json::nlohmann_json ixwebsocket)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(drums_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- tests ----
if(DRUMS_CORE_TESTS)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
        URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        # Only the header and the CTest discovery script are used, not doctest's own CMakeLists.
        SOURCE_SUBDIR do-not-add-subdirectory
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(doctest)

    enable_testing()

    add_executable(drums_core_tests
        tests/test_main.cpp
        tests/test_exercise.cpp
        tests/test_scoring.cpp
        tests/test_midi_recorder.cpp
        tests/test_protocol.cpp
        tests/test_control_server.cpp)
    target_include_directories(drums_core_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
    # The tests use nlohmann to inspect JSON output and an IXWebSocket client.
    target_link_libraries(drums_core_tests PRIVATE drums_core nlohmann_json::nlohmann_json ixwebsocket)
    cmake_path(SET _drums_repo_root NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(drums_core_tests PRIVATE
        DRUMS_REPO_ROOT="${_drums_repo_root}"
        DRUMS_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(drums_core_tests PRIVATE -Wall -Wextra)
    endif()

    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(drums_core_tests PROPERTIES TIMEOUT 60)
endif()
