// Exercise data as used by the audio callback (immutable once handed to the engine).
#pragma once

#include "drums/exercise.h"

#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

namespace drums::engine {

struct RtNote {
    int64_t tick = 0;
    Instrument instrument = Instrument::Unknown;
    int velocity = 100;
};

struct RtExercise {
    int64_t barTicks = 2880;
    int64_t beatTicks = 720;
    int64_t subdivisionTicks = 240;
    int64_t lengthTicks = 2880;
    std::vector<RtNote> notes;  // sorted by tick
};

inline std::unique_ptr<RtExercise> makeRtExercise(const Exercise& ex) {
    auto rt = std::make_unique<RtExercise>();
    rt->barTicks = std::max<int64_t>(1, ex.barTicks());
    rt->beatTicks = std::max(1, ex.beatTicks);
    rt->subdivisionTicks = std::max(1, ex.subdivisionTicks);
    rt->lengthTicks = std::max<int64_t>(1, ex.lengthTicks());
    rt->notes.reserve(ex.notes.size());
    for (const auto& n : ex.notes)
        if (n.instrument != Instrument::Unknown && n.tick >= 0 && n.tick < rt->lengthTicks)
            rt->notes.push_back({n.tick, n.instrument, n.velocity});
    return rt;
}

// Built-in copy of exercises/groove-12-8.json, used when the core library (JSON exercise loader)
// is not linked, and as a fallback.
inline Exercise builtinExercise() {
    Exercise ex;
    ex.id = "groove-12-8-bar23";
    ex.title = "12/8 groove (bar 23)";
    ex.timeSignature = {12, 8};
    ex.ppq = 480;
    ex.beatTicks = 720;
    ex.subdivisionTicks = 240;
    ex.bars = 1;
    ex.defaultBpm = 60.0;
    // gridTicks 240:  hihatClosed "xxx|xxx|xxx|xxx", snare "...|.x.|x..|...", kick "x..|..x|...|..x"
    for (int step = 0; step < 12; ++step) {
        const int64_t tick = step * 240;
        if (step == 0 || step == 5 || step == 11) ex.notes.push_back({Instrument::Kick, tick, 100});
        if (step == 4 || step == 6) ex.notes.push_back({Instrument::Snare, tick, 100});
        ex.notes.push_back({Instrument::HiHatClosed, tick, 100});
    }
    return ex;  // already sorted by tick, then instrument (Kick < Snare < HiHatClosed)
}

}  // namespace drums::engine
