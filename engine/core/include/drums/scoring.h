// Hit scoring. CONTRACT FILE. Implemented in core/src. Not real-time: called from the engine's app thread.
#pragma once

#include "drums/exercise.h"
#include "drums/types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace drums {

struct ScoringConfig {
    double onTimeMs = 15.0;     // |error| <= this -> OnTime
    double maxWindowMs = 150.0; // a hit further than this from every expected note is Extra
    // The window of each expected note is also capped at half the distance to the neighbouring
    // expected note of the same instrument, so adjacent notes never compete for one hit.
};

struct ScoredHit {
    Instrument instrument = Instrument::Unknown;
    int note = 0;
    int velocity = 0;
    int64_t loop = 0;
    double tickInLoop = 0;
    std::optional<int64_t> expectedTick;  // tick in loop of the matched note
    std::optional<double> errorMs;        // negative = early
    Grade grade = Grade::Extra;
};

struct MissedNote {
    Instrument instrument = Instrument::Unknown;
    int64_t loop = 0;
    int64_t expectedTick = 0;
};

struct InstrumentStats {
    int hits = 0;    // matched hits
    int missed = 0;
    int extra = 0;
    double meanErrorMs = 0;  // over matched hits
    double stdDevMs = 0;
};

struct LoopSummary {
    int64_t loop = 0;
    std::map<Instrument, InstrumentStats> perInstrument;  // only instruments with activity
    InstrumentStats total;
};

class Scorer {
public:
    Scorer();
    ~Scorer();
    Scorer(const Scorer&) = delete;
    Scorer& operator=(const Scorer&) = delete;

    void setExercise(const Exercise& exercise);
    void setConfig(const ScoringConfig& config);
    void setLooping(bool looping);  // false: only loop 0 has expected notes
    void reset();                   // on play: forget matches and stats

    // posTicks: latency-compensated transport position of the hit (0 = first tick of the exercise,
    // negative during count-in, keeps growing across loops). msPerTick: at the current tempo.
    ScoredHit onHit(Instrument instrument, int note, int velocity, double posTicks, double msPerTick);

    // Call often (every position update). Returns expected notes whose window closed unmatched.
    std::vector<MissedNote> advance(double posTicks, double msPerTick);

    // Loops whose last window has closed since the previous call.
    std::vector<LoopSummary> takeLoopSummaries();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace drums
