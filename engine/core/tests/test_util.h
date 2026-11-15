// Shared test helpers.
#pragma once

#include "drums/types.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

// doctest stringifies values with an unqualified toString(), which finds drums::toString()
// (returning std::string_view) by ADL and fails to compile. Compare enums as strings instead.
inline std::string S(drums::Grade g) { return std::string(drums::toString(g)); }
inline std::string S(drums::Instrument i) { return std::string(drums::toString(i)); }

namespace test {

inline std::string repoPath(const std::string& relative) { return std::string(DRUMS_REPO_ROOT) + "/" + relative; }

// A fresh, empty directory under the build tree for one test case.
inline std::filesystem::path tempDir(const std::string& name) {
    auto dir = std::filesystem::path(DRUMS_TEST_TMP) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

inline void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace test
