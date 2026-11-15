#include "drums/scoring.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// Model
// -----
// The expected notes form an endless stream: loop L contains every exercise note at
// tick + L * lengthTicks (only loop 0 when not looping). Instances of that stream are generated
// lazily, a little ahead of the latest position seen (by the largest possible window), and are
// kept in `pending_` until their window closes. Each side of a note's window is
// min(maxWindowMs, half the gap to the neighbouring note of the same instrument on that side);
// when looping, the neighbour of the last note of a loop is the first note of the next loop.
//
// Per-loop statistics are accumulated in `accum_` and turned into a LoopSummary once
//   - the transport has passed the end of the loop (so all extras of the loop are in), and
//   - every expected note of the loop has been generated and its window has closed.

namespace drums {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int64_t kNoLimit = std::numeric_limits<int64_t>::max();

struct NoteInfo {
    Instrument instrument = Instrument::Unknown;
    int64_t tick = 0;
    // Half the gap (ticks) to the neighbouring same-instrument note; kInf when there is none.
    double prevHalfOnce = kInf, nextHalfOnce = kInf;          // not looping
    double prevHalfLooped = kInf, nextHalfLooped = kInf;      // looping (wraps around)
};

struct Instance {
    int64_t loop = 0;
    uint32_t index = 0;  // into notes_
    bool matched = false;
};

// Welford's running mean / variance over matched hits, plus counters.
struct StatAccumulator {
    int hits = 0, missed = 0, extra = 0;
    double mean = 0, m2 = 0;

    void addHit(double errorMs) {
        ++hits;
        const double delta = errorMs - mean;
        mean += delta / hits;
        m2 += delta * (errorMs - mean);
    }
    InstrumentStats stats() const {
        InstrumentStats s;
        s.hits = hits;
        s.missed = missed;
        s.extra = extra;
        s.meanErrorMs = hits > 0 ? mean : 0.0;
        s.stdDevMs = hits > 0 ? std::sqrt(std::max(0.0, m2 / hits)) : 0.0;  // population std-dev
        return s;
    }
};

struct LoopAccumulator {
    std::map<Instrument, StatAccumulator> perInstrument;
    StatAccumulator total;
};

}  // namespace

struct Scorer::Impl {
    ScoringConfig config;
    std::vector<NoteInfo> notes;  // sorted by tick, then instrument; no duplicates
    int64_t length = 0;           // ticks per loop; 0 = no exercise
    bool looping = true;
    int64_t loopLimit = kNoLimit;  // last loop with expected notes

    // play state
    std::vector<Instance> pending;  // generated, window not closed yet; in stream order
    int64_t genLoop = 0;            // next instance to generate: (genLoop, genIndex)
    size_t genIndex = 0;
    bool genLoopStarted = false;    // accum entry for genLoop exists
    std::map<int64_t, LoopAccumulator> accum;
    std::vector<LoopSummary> ready;
    double now = -kInf;             // latest advance() position

    void resetPlayState() {
        pending.clear();
        genLoop = 0;
        genIndex = 0;
        genLoopStarted = false;
        accum.clear();
        ready.clear();
        now = -kInf;
        loopLimit = looping ? kNoLimit : 0;
    }

    int64_t loopOf(double posTicks) const {
        return static_cast<int64_t>(std::floor(posTicks / static_cast<double>(length)));
    }
    double absTick(const Instance& p) const {
        return static_cast<double>(p.loop * length + notes[p.index].tick);
    }
    double earlyWindow(const Instance& p, double maxWindowTicks) const {
        const NoteInfo& n = notes[p.index];
        return std::min(maxWindowTicks, loopLimit > 0 ? n.prevHalfLooped : n.prevHalfOnce);
    }
    double lateWindow(const Instance& p, double maxWindowTicks) const {
        const NoteInfo& n = notes[p.index];
        return std::min(maxWindowTicks, p.loop < loopLimit ? n.nextHalfLooped : n.nextHalfOnce);
    }

    // Generate every instance of the stream with absTick <= horizon.
    void generateUpTo(double horizon) {
        while (genLoop <= loopLimit) {
            if (!genLoopStarted) {
                if (static_cast<double>(genLoop * length) > horizon) return;
                accum[genLoop];  // the loop begins: it gets a summary
                genLoopStarted = true;
            }
            if (genIndex >= notes.size()) {
                ++genLoop;
                genIndex = 0;
                genLoopStarted = false;
                continue;
            }
            if (static_cast<double>(genLoop * length + notes[genIndex].tick) > horizon) return;
            pending.push_back({genLoop, static_cast<uint32_t>(genIndex), false});
            ++genIndex;
        }
    }

    StatAccumulator* statsFor(int64_t loop, Instrument instrument, StatAccumulator** total) {
        auto it = accum.find(loop);
        if (it == accum.end()) return nullptr;
        *total = &it->second.total;
        return &it->second.perInstrument[instrument];
    }

    void emitSummaries() {
        while (!accum.empty()) {
            auto it = accum.begin();
            const int64_t loop = it->first;
            if (now < static_cast<double>((loop + 1) * length)) break;  // extras may still come
            if (genLoop <= loop) break;                                  // not fully generated
            if (!pending.empty() && pending.front().loop <= loop) break;  // windows still open
            LoopSummary s;
            s.loop = loop;
            for (const auto& [instrument, acc] : it->second.perInstrument)
                if (acc.hits + acc.missed + acc.extra > 0) s.perInstrument[instrument] = acc.stats();
            s.total = it->second.total.stats();
            ready.push_back(std::move(s));
            accum.erase(it);
        }
    }

    // Looping switched off: no expected notes after the current loop (loop 0 before play).
    void limitLoops() {
        const int64_t current = (length > 0 && std::isfinite(now)) ? loopOf(now) : 0;
        loopLimit = std::max<int64_t>(0, current);
        std::erase_if(pending, [&](const Instance& p) { return p.loop > loopLimit; });
        for (auto it = accum.upper_bound(loopLimit); it != accum.end();) it = accum.erase(it);
        if (genLoop > loopLimit) {
            genLoop = loopLimit + 1;
            genIndex = 0;
            genLoopStarted = false;
        }
    }

    // Looping switched back on: continue the stream, without creating notes in the past.
    void unlimitLoops() {
        if (genLoop > loopLimit && length > 0 && std::isfinite(now)) {
            const int64_t next = loopOf(now) + 1;
            if (next > genLoop) {
                genLoop = next;
                genIndex = 0;
                genLoopStarted = false;
            }
        }
        loopLimit = kNoLimit;
    }
};

Scorer::Scorer() : impl_(std::make_unique<Impl>()) {}
Scorer::~Scorer() = default;

void Scorer::setExercise(const Exercise& exercise) {
    Impl& s = *impl_;
    s.notes.clear();
    s.length = std::max<int64_t>(0, exercise.lengthTicks());

    std::vector<ExerciseNote> sorted;
    for (const ExerciseNote& n : exercise.notes)
        if (n.instrument != Instrument::Unknown && n.tick >= 0 && (s.length == 0 || n.tick < s.length))
            sorted.push_back(n);
    std::stable_sort(sorted.begin(), sorted.end(), [](const ExerciseNote& a, const ExerciseNote& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.instrument < b.instrument;
    });
    for (const ExerciseNote& n : sorted) {
        if (!s.notes.empty() && s.notes.back().tick == n.tick && s.notes.back().instrument == n.instrument)
            continue;  // duplicate: would make a zero-width window
        NoteInfo info;
        info.instrument = n.instrument;
        info.tick = n.tick;
        s.notes.push_back(info);
    }

    // Neighbour gaps per instrument.
    for (Instrument inst : kInstruments) {
        std::vector<size_t> idx;
        for (size_t k = 0; k < s.notes.size(); ++k)
            if (s.notes[k].instrument == inst) idx.push_back(k);
        for (size_t k = 0; k < idx.size(); ++k) {
            NoteInfo& n = s.notes[idx[k]];
            const double wrapGap = static_cast<double>(s.notes[idx.front()].tick + s.length -
                                                       s.notes[idx.back()].tick);
            if (k > 0) {
                n.prevHalfOnce = n.prevHalfLooped = (n.tick - s.notes[idx[k - 1]].tick) / 2.0;
            } else {
                n.prevHalfLooped = wrapGap / 2.0;
            }
            if (k + 1 < idx.size()) {
                n.nextHalfOnce = n.nextHalfLooped = (s.notes[idx[k + 1]].tick - n.tick) / 2.0;
            } else {
                n.nextHalfLooped = wrapGap / 2.0;
            }
        }
    }
    s.resetPlayState();
}

void Scorer::setConfig(const ScoringConfig& config) { impl_->config = config; }

void Scorer::setLooping(bool looping) {
    Impl& s = *impl_;
    if (looping == s.looping) return;
    s.looping = looping;
    if (looping)
        s.unlimitLoops();
    else
        s.limitLoops();
}

void Scorer::reset() { impl_->resetPlayState(); }

ScoredHit Scorer::onHit(Instrument instrument, int note, int velocity, double posTicks, double msPerTick) {
    Impl& s = *impl_;
    ScoredHit hit;
    hit.instrument = instrument;
    hit.note = note;
    hit.velocity = velocity;
    hit.grade = Grade::Extra;
    if (!std::isfinite(posTicks)) return hit;
    if (s.length > 0) {
        hit.loop = s.loopOf(posTicks);
        hit.tickInLoop = posTicks - static_cast<double>(hit.loop * s.length);
    } else {
        hit.loop = 0;
        hit.tickInLoop = posTicks;
        return hit;
    }
    if (!(msPerTick > 0) || instrument == Instrument::Unknown) return hit;

    const double maxWindow = std::max(0.0, s.config.maxWindowMs) / msPerTick;
    s.generateUpTo(posTicks + maxWindow);

    Instance* best = nullptr;
    double bestDistance = kInf;
    for (Instance& p : s.pending) {
        const double expected = s.absTick(p);
        if (expected - maxWindow > posTicks) break;  // stream order: the rest is later still
        if (p.matched || s.notes[p.index].instrument != instrument) continue;
        const double d = posTicks - expected;
        if (d < 0 ? -d > s.earlyWindow(p, maxWindow) : d > s.lateWindow(p, maxWindow)) continue;
        if (std::abs(d) < bestDistance) {  // ties go to the earlier note
            bestDistance = std::abs(d);
            best = &p;
        }
    }

    StatAccumulator* total = nullptr;
    if (best) {
        best->matched = true;
        const double errorMs = (posTicks - s.absTick(*best)) * msPerTick;
        hit.expectedTick = s.notes[best->index].tick;
        hit.errorMs = errorMs;
        hit.grade = std::abs(errorMs) <= s.config.onTimeMs ? Grade::OnTime
                    : errorMs < 0                          ? Grade::Early
                                                           : Grade::Late;
        if (StatAccumulator* acc = s.statsFor(best->loop, instrument, &total)) {
            acc->addHit(errorMs);
            total->addHit(errorMs);
        }
    } else if (StatAccumulator* acc = s.statsFor(hit.loop, instrument, &total)) {
        // Extras count in the loop the hit fell in. Count-in hits (loop -1) and hits in loops
        // without expected notes are not part of any summary.
        ++acc->extra;
        ++total->extra;
    }
    return hit;
}

std::vector<MissedNote> Scorer::advance(double posTicks, double msPerTick) {
    Impl& s = *impl_;
    std::vector<MissedNote> missed;
    if (s.length <= 0 || !(msPerTick > 0) || !std::isfinite(posTicks)) return missed;
    s.now = posTicks;

    const double maxWindow = std::max(0.0, s.config.maxWindowMs) / msPerTick;
    s.generateUpTo(posTicks + maxWindow);

    size_t keep = 0;
    for (size_t k = 0; k < s.pending.size(); ++k) {
        Instance& p = s.pending[k];
        const double expected = s.absTick(p);
        const bool closed = expected <= posTicks && posTicks > expected + s.lateWindow(p, maxWindow);
        if (!closed) {
            s.pending[keep++] = p;
            continue;
        }
        if (!p.matched) {
            const Instrument inst = s.notes[p.index].instrument;
            missed.push_back({inst, p.loop, s.notes[p.index].tick});
            StatAccumulator* total = nullptr;
            if (StatAccumulator* acc = s.statsFor(p.loop, inst, &total)) {
                ++acc->missed;
                ++total->missed;
            }
        }
    }
    s.pending.resize(keep);
    s.emitSummaries();
    return missed;
}

std::vector<LoopSummary> Scorer::takeLoopSummaries() { return std::exchange(impl_->ready, {}); }

}  // namespace drums
