// Exercise model + loading. CONTRACT FILE (see docs/EXERCISE_FORMAT.md). Implemented in core/src.
#pragma once

#include "drums/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace drums {

struct ExerciseNote {
    Instrument instrument = Instrument::Unknown;
    int64_t tick = 0;  // from the start of the exercise; bar = tick / barTicks()
    int velocity = 100;  // 1..127
};

struct Exercise {
    std::string id;
    std::string title;
    TimeSignature timeSignature;
    int ppq = 480;
    int beatTicks = 480;         // what bpm counts (720 = dotted quarter in 12/8)
    int subdivisionTicks = 240;  // metronome subdivision clicks
    int bars = 1;
    double defaultBpm = 80.0;
    std::vector<ExerciseNote> notes;  // sorted by tick, then instrument

    int64_t barTicks() const {
        return int64_t(timeSignature.numerator) * ppq * 4 / timeSignature.denominator;
    }
    int64_t lengthTicks() const { return barTicks() * bars; }
    double msPerTick(double bpm) const { return 60000.0 / (bpm * beatTicks); }
};

// Both throw std::runtime_error with a human-readable message on bad input.
Exercise loadExercise(const std::string& path);
Exercise parseExercise(std::string_view jsonText);

// Canonical JSON object (notes array, no grid) as used in the protocol's "state" message.
std::string exerciseToJson(const Exercise& exercise);

// kits/pad-map.json. loadPadMap throws std::runtime_error; savePadMap returns false on IO error.
PadMap loadPadMap(const std::string& path);
bool savePadMap(const PadMap& map, const std::string& path);

}  // namespace drums
