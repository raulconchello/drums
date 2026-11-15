// Internal helpers shared by the core sources. Not installed, not part of the contract.
#pragma once

#include "drums/exercise.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>

namespace drums::detail {

using ojson = nlohmann::ordered_json;  // keeps "type" first in protocol messages

// v rounded to 1/scale (scale = 10 -> 0.1). Integral results become JSON integers, so 60.0 is
// written as 60. Non-finite values become null.
inline ojson roundedNumber(double v, double scale) {
    if (!std::isfinite(v)) return nullptr;
    // Dividing by an exact integer gives the double closest to the decimal (0.3, not 0.30000000000000004).
    double r = std::round(v * scale) / scale;
    if (r == 0) r = 0;  // no "-0"
    if (std::abs(r) < 9.0e15 && r == std::floor(r)) return static_cast<int64_t>(r);
    return r;
}

// Doubles that came from the user (bpm, volumes): written as is, integral values as integers.
inline ojson plainNumber(double v) {
    if (!std::isfinite(v)) return nullptr;
    if (std::abs(v) < 9.0e15 && v == std::floor(v)) return static_cast<int64_t>(v);
    return v;
}

// Compact text. Invalid UTF-8 (e.g. in a file path) is replaced instead of throwing.
template <class Json>
std::string dump(const Json& j, int indent = -1) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

ojson exerciseToJsonValue(const Exercise& exercise);  // exercise.cpp

}  // namespace drums::detail
