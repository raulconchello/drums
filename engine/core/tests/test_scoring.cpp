#include "drums/scoring.h"

#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace drums;

namespace {

constexpr double kBpm = 60;
const double kMsPerTick = 60000.0 / (kBpm * 720);  // 12/8, bpm counts dotted quarters
constexpr int64_t kLen = 2880;

double ticks(double ms, double msPerTick = kMsPerTick) { return ms / msPerTick; }

Exercise groove() { return loadExercise(test::repoPath("exercises/groove-12-8.json")); }

struct Fixture {
    Exercise exercise = groove();
    Scorer scorer;
    Fixture() {
        scorer.setExercise(exercise);
        scorer.reset();
    }
    ScoredHit hit(Instrument i, double posTicks, double msPerTick = kMsPerTick) {
        return scorer.onHit(i, 38, 100, posTicks, msPerTick);
    }
};

struct Hit {
    double pos;
    Instrument instrument;
};

// Plays `hits` (sorted by time) while advancing the transport in 60 Hz steps from `from` to `to`.
struct Run {
    std::vector<ScoredHit> scored;
    std::vector<MissedNote> missed;
    std::vector<LoopSummary> summaries;
};

// Only the hits in [from, to] are delivered.
Run play(Scorer& scorer, std::vector<Hit> hits, double from, double to, double msPerTick = kMsPerTick) {
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    Run run;
    const double step = (1000.0 / 60.0) / msPerTick;
    size_t next = 0;
    for (double pos = from;; pos += step) {
        const double now = std::min(pos, to);
        for (; next < hits.size() && hits[next].pos <= now; ++next)
            if (hits[next].pos >= from)
                run.scored.push_back(scorer.onHit(hits[next].instrument, 0, 100, hits[next].pos, msPerTick));
        auto m = scorer.advance(now, msPerTick);
        run.missed.insert(run.missed.end(), m.begin(), m.end());
        auto s = scorer.takeLoopSummaries();
        run.summaries.insert(run.summaries.end(), s.begin(), s.end());
        if (now >= to) break;
    }
    return run;
}

}  // namespace

TEST_CASE("scoring: on time, early, late") {
    Fixture f;
    ScoredHit h = f.hit(Instrument::Kick, ticks(5));
    CHECK(S(h.grade) == S(Grade::OnTime));
    REQUIRE(h.expectedTick);
    CHECK(*h.expectedTick == 0);
    REQUIRE(h.errorMs);
    CHECK(*h.errorMs == doctest::Approx(5));
    CHECK(h.loop == 0);
    CHECK(h.tickInLoop == doctest::Approx(ticks(5)));
    CHECK(S(h.instrument) == S(Instrument::Kick));
    CHECK(h.note == 38);
    CHECK(h.velocity == 100);

    h = f.hit(Instrument::HiHatClosed, -ticks(14.9));  // just inside the on-time limit
    CHECK(S(h.grade) == S(Grade::OnTime));
    CHECK(*h.errorMs == doctest::Approx(-14.9));

    h = f.hit(Instrument::Snare, 960 - ticks(40));
    CHECK(S(h.grade) == S(Grade::Early));
    CHECK(*h.expectedTick == 960);
    CHECK(*h.errorMs == doctest::Approx(-40));

    h = f.hit(Instrument::HiHatClosed, 240 + ticks(60));
    CHECK(S(h.grade) == S(Grade::Late));
    CHECK(*h.expectedTick == 240);
    CHECK(*h.errorMs == doctest::Approx(60));

    h = f.hit(Instrument::Snare, 1440 + ticks(15.5));
    CHECK(S(h.grade) == S(Grade::Late));
}

TEST_CASE("scoring: extra hits") {
    Fixture f;
    // Far from any snare note.
    ScoredHit h = f.hit(Instrument::Snare, 200);
    CHECK(S(h.grade) == S(Grade::Extra));
    CHECK_FALSE(h.expectedTick);
    CHECK_FALSE(h.errorMs);
    CHECK(h.loop == 0);
    CHECK(h.tickInLoop == doctest::Approx(200));

    // An instrument that is not in the exercise.
    CHECK(S(f.hit(Instrument::Crash, 0).grade) == S(Grade::Extra));
    // Just outside the 150 ms window.
    CHECK(S(f.hit(Instrument::Snare, 960 + ticks(151)).grade) == S(Grade::Extra));
    // A note matches only once: the second hit is extra (the next hihat is too far).
    CHECK(S(f.hit(Instrument::HiHatClosed, 240 + ticks(20)).grade) == S(Grade::Late));
    h = f.hit(Instrument::HiHatClosed, 240 + ticks(25));
    CHECK(S(h.grade) == S(Grade::Extra));
    // A hit matches the nearest unmatched note: 2 hits around 480/720 go to each.
    h = f.hit(Instrument::HiHatClosed, 480 + ticks(10));
    CHECK(*h.expectedTick == 480);
    h = f.hit(Instrument::HiHatClosed, 720 - ticks(100));
    CHECK(*h.expectedTick == 720);
    CHECK(S(h.grade) == S(Grade::Early));
}

TEST_CASE("scoring: missed notes via advance") {
    Fixture f;
    CHECK(f.scorer.advance(-500, kMsPerTick).empty());
    CHECK(f.scorer.advance(100, kMsPerTick).empty());  // windows of tick 0 still open (108 ticks)
    f.hit(Instrument::Kick, 2);                         // kick 0 is played

    auto missed = f.scorer.advance(ticks(150) + 0.5, kMsPerTick);
    REQUIRE(missed.size() == 1);
    CHECK(S(missed[0].instrument) == S(Instrument::HiHatClosed));
    CHECK(missed[0].loop == 0);
    CHECK(missed[0].expectedTick == 0);

    // Everything up to (and including) tick 960 for snare / 1200 for kick, in stream order.
    missed = f.scorer.advance(1200 + ticks(150) + 0.5, kMsPerTick);
    std::vector<std::pair<Instrument, int64_t>> got;
    for (auto& m : missed) got.emplace_back(m.instrument, m.expectedTick);
    const std::vector<std::pair<Instrument, int64_t>> expected = {
        {Instrument::HiHatClosed, 240}, {Instrument::HiHatClosed, 480}, {Instrument::HiHatClosed, 720},
        {Instrument::HiHatClosed, 960}, {Instrument::Snare, 960},      {Instrument::HiHatClosed, 1200},
        {Instrument::Kick, 1200},
    };
    // Same set; order is by expected time.
    CHECK(got.size() == expected.size());
    for (auto& e : expected) CHECK(std::find(got.begin(), got.end(), e) != got.end());
    for (size_t k = 1; k < missed.size(); ++k) CHECK(missed[k - 1].expectedTick <= missed[k].expectedTick);

    // Reported once only.
    CHECK(f.scorer.advance(1200 + ticks(150) + 1, kMsPerTick).empty());
    // A hit in a window that was already closed and reported is extra.
    CHECK(S(f.hit(Instrument::Snare, 960 + ticks(100)).grade) == S(Grade::Extra));
}

TEST_CASE("scoring: two loops with loop summaries") {
    Fixture f;
    const Exercise& ex = f.exercise;
    std::vector<Hit> hits;
    // Loop 0: every note 12 ms late.
    for (const auto& n : ex.notes) hits.push_back({double(n.tick) + ticks(12), n.instrument});
    // Loop 1: skip the snare at 960, alternate +20 / -20 ms, and one extra snare at 2000.
    int k = 0;
    for (const auto& n : ex.notes) {
        if (n.instrument == Instrument::Snare && n.tick == 960) continue;
        const double err = (k++ % 2 == 0) ? 20 : -20;
        hits.push_back({double(kLen + n.tick) + ticks(err), n.instrument});
    }
    hits.push_back({double(kLen + 2000), Instrument::Snare});

    // Advance up to just before the end of loop 0: no summary yet.
    Run first = play(f.scorer, hits, -1440, kLen - 1);
    CHECK(first.summaries.empty());
    CHECK(first.missed.empty());

    // Up to loop 2's first windows, still open (5760 + 108 ticks).
    Run rest = play(f.scorer, hits, kLen, 2 * kLen + 100);
    REQUIRE(rest.summaries.size() == 2);

    const LoopSummary& s0 = rest.summaries[0];
    CHECK(s0.loop == 0);
    CHECK(s0.total.hits == 17);
    CHECK(s0.total.missed == 0);
    CHECK(s0.total.extra == 0);
    CHECK(s0.total.meanErrorMs == doctest::Approx(12));
    CHECK(s0.total.stdDevMs == doctest::Approx(0).epsilon(1e-6));
    CHECK(s0.perInstrument.size() == 3);
    CHECK(s0.perInstrument.at(Instrument::HiHatClosed).hits == 12);
    CHECK(s0.perInstrument.at(Instrument::Snare).hits == 2);
    CHECK(s0.perInstrument.at(Instrument::Kick).hits == 3);
    CHECK(s0.perInstrument.at(Instrument::Kick).meanErrorMs == doctest::Approx(12));

    const LoopSummary& s1 = rest.summaries[1];
    CHECK(s1.loop == 1);
    CHECK(s1.total.hits == 16);
    CHECK(s1.total.missed == 1);
    CHECK(s1.total.extra == 1);
    CHECK(s1.total.meanErrorMs == doctest::Approx(0).epsilon(1e-6));
    CHECK(s1.total.stdDevMs == doctest::Approx(20));
    const InstrumentStats& snare = s1.perInstrument.at(Instrument::Snare);
    CHECK(snare.hits == 1);
    CHECK(snare.missed == 1);
    CHECK(snare.extra == 1);

    REQUIRE(rest.missed.size() == 1);
    CHECK(S(rest.missed[0].instrument) == S(Instrument::Snare));
    CHECK(rest.missed[0].loop == 1);
    CHECK(rest.missed[0].expectedTick == 960);

    // Each loop is summarised once.
    CHECK(f.scorer.takeLoopSummaries().empty());
    f.scorer.advance(2 * kLen + 300, kMsPerTick);
    CHECK(f.scorer.takeLoopSummaries().empty());

    // The extra reports loop / tickInLoop from its posTicks.
    const auto extra = std::find_if(rest.scored.begin(), rest.scored.end(),
                                    [](const ScoredHit& h) { return h.grade == Grade::Extra; });
    REQUIRE(extra != rest.scored.end());
    CHECK(extra->loop == 1);
    CHECK(extra->tickInLoop == doctest::Approx(2000));
}

TEST_CASE("scoring: loop summary waits for the last window and the end of the loop") {
    Fixture f;
    f.scorer.advance(kLen - 1, kMsPerTick);
    CHECK(f.scorer.takeLoopSummaries().empty());
    f.scorer.advance(kLen, kMsPerTick);  // last windows (2640 + 108 ticks) closed, loop ended
    auto s = f.scorer.takeLoopSummaries();
    REQUIRE(s.size() == 1);
    CHECK(s[0].loop == 0);
    CHECK(s[0].total.missed == 17);
    CHECK(s[0].total.hits == 0);
    CHECK(s[0].total.meanErrorMs == 0);
    CHECK(s[0].total.stdDevMs == 0);

    // An exercise whose last note's window reaches into the next loop.
    Exercise ex = parseExercise(R"({"timeSignature": [4, 4], "notes": [{"instrument": "ride", "tick": 1900}]})");
    Scorer scorer;
    scorer.setExercise(ex);
    scorer.reset();
    const double mpt = ex.msPerTick(60);  // 2.083 ms/tick: 150 ms = 72 ticks
    scorer.advance(1920, mpt);
    CHECK(scorer.takeLoopSummaries().empty());
    CHECK(scorer.advance(1900 + 71, mpt).empty());
    CHECK(scorer.takeLoopSummaries().empty());
    auto missed = scorer.advance(1900 + 73, mpt);
    CHECK(missed.size() == 1);
    s = scorer.takeLoopSummaries();
    REQUIRE(s.size() == 1);
    CHECK(s[0].total.missed == 1);
}

TEST_CASE("scoring: early hit during the count-in matches loop 0") {
    Fixture f;
    f.scorer.advance(-1440, kMsPerTick);
    ScoredHit h = f.hit(Instrument::HiHatClosed, -ticks(30));
    CHECK(S(h.grade) == S(Grade::Early));
    CHECK(*h.expectedTick == 0);
    CHECK(*h.errorMs == doctest::Approx(-30));
    CHECK(h.loop == -1);  // from posTicks: floor(-21.6 / 2880)
    CHECK(h.tickInLoop == doctest::Approx(kLen - ticks(30)));

    // A count-in hit far from the first notes is extra, and not part of loop 0's summary.
    h = f.hit(Instrument::Snare, -1000);
    CHECK(S(h.grade) == S(Grade::Extra));
    CHECK(h.loop == -1);
    CHECK(h.tickInLoop == doctest::Approx(1880));

    f.scorer.advance(kLen, kMsPerTick);
    auto s = f.scorer.takeLoopSummaries();
    REQUIRE(s.size() == 1);
    CHECK(s[0].total.hits == 1);
    CHECK(s[0].total.extra == 0);
    CHECK(s[0].total.missed == 16);
    CHECK(s[0].total.meanErrorMs == doctest::Approx(-30));
}

TEST_CASE("scoring: not looping") {
    Fixture f;
    f.scorer.setLooping(false);
    f.scorer.reset();

    // The last hihat (2640) has no neighbour after it: full window, 100 ms late still counts.
    ScoredHit h = f.hit(Instrument::HiHatClosed, 2640 + ticks(100));
    CHECK(S(h.grade) == S(Grade::Late));
    CHECK(*h.expectedTick == 2640);
    // No loop 1: a hit on its first note is extra.
    h = f.hit(Instrument::Kick, kLen + 1);
    CHECK(S(h.grade) == S(Grade::Extra));
    CHECK(h.loop == 1);
    CHECK(h.tickInLoop == doctest::Approx(1));

    auto missed = f.scorer.advance(10 * kLen, kMsPerTick);
    CHECK(missed.size() == 16);
    for (auto& m : missed) CHECK(m.loop == 0);
    auto s = f.scorer.takeLoopSummaries();
    REQUIRE(s.size() == 1);
    CHECK(s[0].loop == 0);
    CHECK(s[0].total.hits == 1);
    CHECK(s[0].total.missed == 16);
    CHECK(s[0].total.extra == 0);  // the loop-1 extra belongs to no summary
    CHECK(f.scorer.advance(20 * kLen, kMsPerTick).empty());
    CHECK(f.scorer.takeLoopSummaries().empty());

    // Looping (the default): the hit just after the loop end is loop 1's kick, early side...
    Fixture looped;
    h = looped.hit(Instrument::Kick, kLen - ticks(10));
    CHECK(S(h.grade) == S(Grade::OnTime));
    CHECK(*h.expectedTick == 0);
    CHECK(h.loop == 0);
}

TEST_CASE("scoring: switching looping off mid-play finishes the current loop") {
    Fixture f;
    f.scorer.advance(2 * kLen + 100, kMsPerTick);  // in loop 2
    f.scorer.takeLoopSummaries();
    f.scorer.setLooping(false);
    auto missed = f.scorer.advance(6 * kLen, kMsPerTick);
    for (auto& m : missed) CHECK(m.loop == 2);
    auto s = f.scorer.takeLoopSummaries();
    REQUIRE(s.size() == 1);
    CHECK(s[0].loop == 2);
}

TEST_CASE("scoring: windows are capped at half the gap to the neighbouring note") {
    SUBCASE("hihat eighths at 180 bpm") {
        Fixture f;
        const double mpt = f.exercise.msPerTick(180);  // 0.463 ms/tick: 150 ms = 324 ticks, half gap 120
        ScoredHit h = f.hit(Instrument::HiHatClosed, 240 + 100, mpt);
        CHECK(S(h.grade) == S(Grade::Late));
        CHECK(*h.expectedTick == 240);
        CHECK(*h.errorMs == doctest::Approx(100 * mpt));

        h = f.hit(Instrument::HiHatClosed, 240 + 130, mpt);  // past 240's window: early for 480
        CHECK(S(h.grade) == S(Grade::Early));
        CHECK(*h.expectedTick == 480);
        CHECK(*h.errorMs == doctest::Approx(-110 * mpt));

        // Kick 2640 -> next kick at 2880 (loop 1, tick 0): 130 ticks late is early for loop 1.
        h = f.hit(Instrument::Kick, 2640 + 130, mpt);
        CHECK(*h.expectedTick == 0);
        CHECK(S(h.grade) == S(Grade::Early));
        CHECK(h.loop == 0);
        CHECK(h.tickInLoop == doctest::Approx(2770));
        // Kick 1200 has gaps of 1200 and 1440 ticks: the 150 ms (324 ticks) limit applies.
        h = f.hit(Instrument::Kick, 1200 - 330, mpt);
        CHECK(S(h.grade) == S(Grade::Extra));
        h = f.hit(Instrument::Kick, 1200 + 300, mpt);
        CHECK(S(h.grade) == S(Grade::Late));
        CHECK(*h.expectedTick == 1200);
    }
    SUBCASE("two snares 60 ticks apart at 60 bpm") {
        Exercise ex = groove();
        ex.notes = {{Instrument::Snare, 960, 100}, {Instrument::Snare, 1020, 100}};
        Scorer scorer;
        scorer.setExercise(ex);
        scorer.reset();
        // Windows: 960 -> [.., 990], 1020 -> [990, ..]
        ScoredHit h = scorer.onHit(Instrument::Snare, 38, 100, 960 + 25, kMsPerTick);
        CHECK(*h.expectedTick == 960);
        CHECK(S(h.grade) == S(Grade::Late));
        // 960 is taken; 988 is 32 ticks before 1020, outside its 30-tick early window.
        h = scorer.onHit(Instrument::Snare, 38, 100, 960 + 28, kMsPerTick);
        CHECK(S(h.grade) == S(Grade::Extra));
        h = scorer.onHit(Instrument::Snare, 38, 100, 1020 - 29, kMsPerTick);
        CHECK(*h.expectedTick == 1020);
        CHECK(*h.errorMs == doctest::Approx(-29 * kMsPerTick));
    }
}

TEST_CASE("scoring: loop and tickInLoop from posTicks") {
    Fixture f;
    ScoredHit h = f.hit(Instrument::Crash, -10);
    CHECK(h.loop == -1);
    CHECK(h.tickInLoop == doctest::Approx(2870));
    h = f.hit(Instrument::Crash, 2 * kLen + 10.5);
    CHECK(h.loop == 2);
    CHECK(h.tickInLoop == doctest::Approx(10.5));
    h = f.hit(Instrument::Crash, -kLen);
    CHECK(h.loop == -1);
    CHECK(h.tickInLoop == doctest::Approx(0));

    // A late hit for the last note of a loop that falls in the next loop: loop / tickInLoop are
    // where the hit fell, expectedTick is the note's tick in its own loop.
    Exercise ex = parseExercise(R"({"timeSignature": [4, 4], "notes": [{"instrument": "ride", "tick": 1900}]})");
    Scorer scorer;
    scorer.setExercise(ex);
    scorer.reset();
    h = scorer.onHit(Instrument::Ride, 51, 90, 1920 + 1900 + 50, ex.msPerTick(60));
    CHECK(S(h.grade) == S(Grade::Late));
    CHECK(*h.expectedTick == 1900);
    CHECK(*h.errorMs == doctest::Approx(50 * ex.msPerTick(60)));
    CHECK(h.loop == 2);
    CHECK(h.tickInLoop == doctest::Approx(30));
}

TEST_CASE("scoring: reset and no exercise") {
    Scorer empty;
    ScoredHit h = empty.onHit(Instrument::Kick, 36, 100, 100, kMsPerTick);
    CHECK(S(h.grade) == S(Grade::Extra));
    CHECK(empty.advance(1000, kMsPerTick).empty());
    CHECK(empty.takeLoopSummaries().empty());

    Fixture f;
    CHECK(S(f.hit(Instrument::Kick, 0).grade) == S(Grade::OnTime));
    CHECK(S(f.hit(Instrument::Kick, 1).grade) == S(Grade::Extra));
    f.scorer.reset();
    CHECK(S(f.hit(Instrument::Kick, 1).grade) == S(Grade::OnTime));

    ScoringConfig config;
    config.onTimeMs = 5;
    config.maxWindowMs = 50;
    f.scorer.setConfig(config);
    CHECK(S(f.hit(Instrument::Snare, 960 + ticks(10)).grade) == S(Grade::Late));
    CHECK(S(f.hit(Instrument::Snare, 1440 - ticks(60)).grade) == S(Grade::Extra));
}

TEST_CASE("scoring: 40 minutes of looping at 60 Hz stays fast") {
    Fixture f;
    const double step = ticks(1000.0 / 60);
    const int loops = 600;  // 600 x 4 s = 40 minutes of play
    size_t summaries = 0, missed = 0, scored = 0;
    auto start = std::chrono::steady_clock::now();
    size_t noteIndex = 0;
    int64_t loop = 0;
    for (double pos = -1440; pos < loops * double(kLen); pos += step) {
        // Play every other loop.
        while (true) {
            const auto& n = f.exercise.notes[noteIndex];
            const double t = double(loop * kLen + n.tick);
            if (t > pos) break;
            if (loop % 2 == 0) {
                f.hit(n.instrument, t + 3);
                ++scored;
            }
            if (++noteIndex == f.exercise.notes.size()) {
                noteIndex = 0;
                ++loop;
            }
        }
        missed += f.scorer.advance(pos, kMsPerTick).size();
        summaries += f.scorer.takeLoopSummaries().size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(summaries == loops - 1);
    CHECK(missed == loops / 2 * 17);  // every note of the odd loops
    CHECK(scored == loops / 2 * 17);
    MESSAGE("40 min of play scored in " << std::chrono::duration<double, std::milli>(elapsed).count() << " ms");
    CHECK(elapsed < std::chrono::seconds(2));
}
