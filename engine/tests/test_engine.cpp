// Unit tests of the real-time engine, run offline (no JACK): metronome/groove schedules are
// sample-exact and independent of the block size, tempo changes, end of exercise, MIDI handling
// (clock ignored, velocity-0 note-on = note-off, CC), hit time-stamps with latency compensation,
// per-instrument routing/volume and sample-accurate pad triggering through sfizz.
#include "audio_engine.h"
#include "exercise_rt.h"
#include "kit.h"
#include "session.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace drums;
using namespace drums::engine;

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++gChecks;                                                                        \
        if (!(cond)) {                                                                    \
            ++gFailures;                                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                 \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++gChecks;                                                                                         \
        const auto va_ = (a);                                                                              \
        const auto vb_ = (b);                                                                              \
        if (!(va_ == vb_)) {                                                                               \
            ++gFailures;                                                                                   \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                         (long long)va_, (long long)vb_);                                                  \
        }                                                                                                  \
    } while (0)

constexpr int kRate = 48000;

static std::string kitPath() {
    return (std::filesystem::path(DRUMS_REPO_ROOT) / "kits" / "avl-black-pearl" / "kit.json").string();
}

static PadMap testPadMap() {
    PadMap m;
    m.byNote[36] = Instrument::Kick;
    m.byNote[38] = Instrument::Snare;
    m.byNote[42] = Instrument::HiHatClosed;
    return m;
}

struct Run {
    std::vector<TraceEvent> trace;
    std::vector<Event> events;
    std::vector<float> left, right;
};

// Renders `frames` with the given block-size generator; `midiAt` injects MIDI per block.
static Run render(AudioEngine& e, int64_t frames, const std::function<int(int)>& blockSize,
                  const std::function<std::vector<MidiEvent>(int64_t, int)>& midiAt = nullptr,
                  const std::function<void(int64_t)>& beforeBlock = nullptr) {
    Run r;
    r.left.resize(size_t(frames));
    r.right.resize(size_t(frames));
    int64_t done = 0;
    for (int i = 0; done < frames; ++i) {
        const int n = int(std::min<int64_t>(blockSize(i), frames - done));
        if (beforeBlock) beforeBlock(done);
        std::vector<MidiEvent> midi = midiAt ? midiAt(done, n) : std::vector<MidiEvent>{};
        e.process(n, midi.data(), int(midi.size()), r.left.data() + done, r.right.data() + done);
        done += n;
        Event ev;
        while (e.poll(ev)) {
            if (ev.type == EventType::Garbage) AudioEngine::dispose(ev);
            else r.events.push_back(ev);
        }
    }
    TraceEvent t;
    while (e.pollTrace(t)) r.trace.push_back(t);
    return r;
}

static std::unique_ptr<AudioEngine> makeEngine(bool groove, bool loop = true, int countIn = 1) {
    auto e = std::make_unique<AudioEngine>(kRate, true);
    e->installExercise(makeRtExercise(builtinExercise()));
    e->send(Command::bpm(60));
    e->send(Command::countIn(countIn));
    e->send(Command::loop(loop));
    e->send(Command::click(true, 0.8, true));
    e->send(Command::groove(groove, 0.7));
    return e;
}

// 60 bpm, 12/8, beatTicks 720: 1 tick = 66.67 frames, subdivision (240) = 16000 frames = 1/3 s,
// beat (720) = 48000 = 1 s, bar (2880) = 192000 = 4 s.
static void testClickSchedule() {
    std::printf("click schedule (60 bpm 12/8, count-in 1 bar, subdivisions)\n");
    std::vector<std::vector<TraceEvent>> runs;
    const std::vector<std::function<int(int)>> sizes = {
        [](int) { return 128; }, [](int) { return 1000; }, [](int) { return 2048 + 37; },
        [](int i) { return 1 + (i * 7919) % 700; }};
    for (const auto& size : sizes) {
        auto e = makeEngine(false);
        e->send(Command::play());
        runs.push_back(render(*e, 20 * kRate, size).trace);
    }
    const auto& t = runs[0];
    CHECK_EQ(t.size(), size_t(60));  // 20 s of 1/3 s subdivisions
    int accents = 0, beats = 0, subs = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        CHECK_EQ(t[i].frame, int64_t(i) * 16000);
        const int inBar = int(i % 12);
        const TraceKind expect =
            inBar == 0 ? TraceKind::Accent : (inBar % 3 == 0 ? TraceKind::Beat : TraceKind::Subdivision);
        CHECK(t[i].kind == expect);
        accents += t[i].kind == TraceKind::Accent;
        beats += t[i].kind == TraceKind::Beat;
        subs += t[i].kind == TraceKind::Subdivision;
    }
    CHECK_EQ(accents, 5);
    CHECK_EQ(beats, 15);
    CHECK_EQ(subs, 40);
    CHECK_EQ(t[0].tick, -2880.0);  // count-in starts one bar before 0
    CHECK_EQ(t[12].tick, 0.0);
    for (size_t r = 1; r < runs.size(); ++r) {
        CHECK_EQ(runs[r].size(), t.size());
        for (size_t i = 0; i < std::min(t.size(), runs[r].size()); ++i) CHECK_EQ(runs[r][i].frame, t[i].frame);
    }
    std::printf("  %zu clicks, %d accents / %d beats / %d subdivisions, identical for 4 block-size patterns\n",
                t.size(), accents, beats, subs);
}

static void testGrooveAndEnd() {
    std::printf("groove schedule + loop/no-loop\n");
    {
        auto e = makeEngine(true);
        // Groove needs a kit to be scheduled; with no kit nothing must be played or traced.
        e->send(Command::play());
        auto r = render(*e, 6 * kRate, [](int) { return 256; });
        int groove = 0;
        for (const auto& t : r.trace) groove += t.kind == TraceKind::Groove;
        CHECK_EQ(groove, 0);
    }
    if (!std::filesystem::exists(kitPath())) {
        std::printf("  (kit missing, groove timing skipped: run kits/fetch-kits.sh)\n");
        return;
    }
    std::string err;
    const KitConfig kit = loadKitConfig(kitPath());
    for (bool loop : {true, false}) {
        auto e = makeEngine(true, loop);
        e->installKit(loadKitBundle(kit, kRate, true, &err));
        e->send(Command::play());
        auto r = render(*e, 13 * kRate, [](int i) { return 64 + (i * 31) % 300; });
        const Exercise ex = builtinExercise();
        std::vector<int64_t> expected;
        for (int64_t l = 0; l < (loop ? 3 : 1); ++l)
            for (const auto& n : ex.notes) {
                const int64_t f = 192000 + l * 192000 + n.tick * 200 / 3;  // tick * 66.67
                if (f < 13 * kRate) expected.push_back(f);
            }
        std::vector<int64_t> got;
        for (const auto& t : r.trace)
            if (t.kind == TraceKind::Groove) got.push_back(t.frame);
        CHECK_EQ(got.size(), expected.size());
        bool same = got == expected;
        CHECK(same);
        int stopped = 0;
        int64_t stopFrame = -1;
        for (const auto& ev : r.events)
            if (ev.type == EventType::Stopped) ++stopped, stopFrame = ev.frame;
        int64_t lastClick = 0;
        for (const auto& t : r.trace)
            if (t.kind <= TraceKind::Subdivision) lastClick = t.frame;
        if (loop) {
            CHECK_EQ(stopped, 0);
        } else {
            CHECK_EQ(stopped, 1);
            CHECK_EQ(stopFrame, int64_t(384000));  // count-in bar + 1 exercise bar
            CHECK_EQ(lastClick, int64_t(384000 - 16000));
        }
        std::printf("  loop=%d: %zu groove notes at exact frames (%s), stopped=%d at %lld\n", loop, got.size(),
                    same ? "match" : "MISMATCH", stopped, static_cast<long long>(stopFrame));
    }
    // Groove audio, click off: silence through the count-in, first sound exactly at frame 192000.
    auto e = makeEngine(true, true);
    e->send(Command::click(false, std::nullopt, std::nullopt));
    e->installKit(loadKitBundle(kit, kRate, true, &err));
    e->send(Command::play());
    auto r = render(*e, 5 * kRate, [](int) { return 128; });
    int first = -1;
    for (size_t f = 0; f < r.left.size() && first < 0; ++f)
        if (r.left[f] != 0.0f || r.right[f] != 0.0f) first = int(f);
    float peak = 0;
    for (int f = 192000; f < 192000 + 4800; ++f) peak = std::max(peak, std::fabs(r.left[size_t(f)]));
    CHECK_EQ(first, 192000);
    CHECK(peak > 0.01f);
    std::printf("  groove only (click off): first non-zero sample at frame %d (expected 192000), peak %.3f\n", first,
                peak);
}

static void testTempoChange() {
    std::printf("tempo change at a buffer boundary\n");
    auto e = makeEngine(false, true, 0);
    e->send(Command::play());
    // 60 bpm for 2 s (= 6 subdivisions at 0..80000), then 120 bpm from frame 96000 (a block boundary).
    auto r = render(*e, 4 * kRate, [](int) { return 128; }, nullptr, [&](int64_t done) {
        if (done == 96000) e->send(Command::bpm(120));
    });
    std::vector<int64_t> frames;
    for (const auto& t : r.trace) frames.push_back(t.frame);
    // 0,16000,...,80000 at 60 bpm; at 96000 position = 1440 ticks = a subdivision; then every 8000.
    std::vector<int64_t> expected = {0, 16000, 32000, 48000, 64000, 80000};
    for (int64_t f = 96000; f < 4 * kRate; f += 8000) expected.push_back(f);
    CHECK(frames == expected);
    std::printf("  %zu clicks, spacing 16000 -> 8000 frames at frame 96000: %s\n", frames.size(),
                frames == expected ? "ok" : "MISMATCH");
}

static void testMidi() {
    std::printf("MIDI handling + hit time-stamps\n");
    auto e = makeEngine(false, true, 1);
    sendPadMap(*e, testPadMap());
    e->setOutputLatencyFrames(256);
    e->send(Command::latencyOffset(4.0));
    // Before play: one hit while stopped -> Free.
    auto stoppedRun = render(*e, 1024, [](int) { return 512; }, [](int64_t done, int) {
        std::vector<MidiEvent> m;
        if (done == 0) m.push_back({10, 3, {0x99, 38, 90}});
        return m;
    });
    int hits = 0;
    for (const auto& ev : stoppedRun.events)
        if (ev.type == EventType::Hit) {
            ++hits;
            CHECK(!ev.playing);
            CHECK(ev.instrument == Instrument::Snare);
            CHECK_EQ(ev.frame, int64_t(10));
        }
    CHECK_EQ(hits, 1);

    e->send(Command::play());  // applied at the next block start: frame 1024
    const int64_t playFrame = 1024;
    auto r = render(*e, 5 * kRate, [](int) { return 128; }, [&](int64_t done, int n) {
        std::vector<MidiEvent> m;
        const int64_t abs = playFrame + done;
        // a stream of MIDI clock in every block
        m.push_back({0, 1, {0xF8, 0, 0}});
        if (abs <= 200000 && 200000 < abs + n) {
            const uint32_t off = uint32_t(200000 - abs);
            m.push_back({off, 1, {0xF8, 0, 0}});
            m.push_back({off, 3, {0x99, 38, 100}});  // snare on channel 10
        }
        if (abs <= 210037 && 210037 < abs + n) {
            const uint32_t off = uint32_t(210037 - abs);
            m.push_back({off, 3, {0x99, 38, 0}});    // note-on velocity 0 = note-off
            m.push_back({off, 3, {0xB9, 4, 77}});    // hi-hat pedal CC
            m.push_back({off, 3, {0x99, 99, 64}});   // unmapped pad
        }
        if (abs <= 210100 && 210100 < abs + n) m.push_back({uint32_t(210100 - abs), 3, {0x89, 36, 64}});
        return m;
    });
    std::vector<Event> hitEvents, offs, ccs;
    for (const auto& ev : r.events) {
        if (ev.type == EventType::Hit) hitEvents.push_back(ev);
        if (ev.type == EventType::NoteOff) offs.push_back(ev);
        if (ev.type == EventType::Control) ccs.push_back(ev);
    }
    CHECK_EQ(hitEvents.size(), size_t(2));
    CHECK_EQ(offs.size(), size_t(2));
    CHECK_EQ(ccs.size(), size_t(1));
    if (hitEvents.size() == 2) {
        const Event& h = hitEvents[0];
        CHECK(h.playing);
        CHECK(h.instrument == Instrument::Snare);
        CHECK_EQ(h.note, 38);
        CHECK_EQ(h.velocity, 100);
        CHECK_EQ(h.frame, int64_t(200000));
        // raw position: -2880 + (200000-1024) frames * 0.015 ticks/frame; compensation 256 frames
        // (5.333 ms) + 4 ms = 9.333 ms = 6.72 ticks at 1.389 ms/tick.
        const double msPerTick = 60000.0 / (60 * 720);
        const double raw = -2880.0 + (200000 - 1024) / (msPerTick * 48.0);
        const double expected = raw - (256.0 / 48.0 + 4.0) / msPerTick;
        CHECK(std::fabs(h.posTicks - expected) < 1e-6);
        CHECK(std::fabs(h.msPerTick - msPerTick) < 1e-12);
        std::printf("  hit frame %lld posTicks %.4f (expected %.4f, raw %.4f, compensation 9.333 ms)\n",
                    static_cast<long long>(h.frame), h.posTicks, expected, raw);
        CHECK(hitEvents[1].instrument == Instrument::Unknown);
        CHECK_EQ(hitEvents[1].note, 99);
    }
    if (offs.size() == 2) {
        CHECK_EQ(offs[0].frame, int64_t(210037));
        CHECK_EQ(offs[0].note, 38);
        CHECK_EQ(offs[1].note, 36);
    }
    if (ccs.size() == 1) {
        CHECK_EQ(ccs[0].controller, 4);
        CHECK_EQ(ccs[0].value, 77);
    }
}

static int firstAbove(const std::vector<float>& x, int64_t from, float threshold) {
    for (size_t i = size_t(from); i < x.size(); ++i)
        if (std::fabs(x[i]) > threshold) return int(i);
    return -1;
}

static void testPadsThroughSampler() {
    std::printf("pads through sfizz: sample-accurate onset, per-instrument volume, velocity curve\n");
    if (!std::filesystem::exists(kitPath())) {
        std::printf("  (kit missing: skipped)\n");
        return;
    }
    std::string err;
    const KitConfig kit = loadKitConfig(kitPath());
    PadMap pads;
    for (Instrument i : kInstruments) pads.byNote[size_t(kit.notes[instrumentIndex(i)])] = i;

    // One hit per instrument at offset 37 of a block; measure where the output becomes non-silent.
    for (Instrument i : kInstruments) {
        auto e = std::make_unique<AudioEngine>(kRate, true);
        e->installExercise(makeRtExercise(builtinExercise()));
        e->installKit(loadKitBundle(kit, kRate, true, &err));
        sendPadMap(*e, pads);
        const int note = kit.notes[instrumentIndex(i)];
        auto r = render(*e, 9600, [](int) { return 128; }, [&](int64_t done, int) {
            std::vector<MidiEvent> m;
            if (done == 1024) m.push_back({37, 3, {0x99, uint8_t(note), 110}});
            return m;
        });
        bool silentBefore = true;
        for (int f = 0; f < 1024 + 37; ++f) silentBefore &= r.left[size_t(f)] == 0.0f && r.right[size_t(f)] == 0.0f;
        const int any = std::max(firstAbove(r.left, 0, 0.0f), firstAbove(r.right, 0, 0.0f));
        const int audible = std::max(firstAbove(r.left, 0, 1e-3f), firstAbove(r.right, 0, 1e-3f));
        float peak = 0;
        for (float v : r.left) peak = std::max(peak, std::fabs(v));
        CHECK(silentBefore);
        CHECK(any >= 1024 + 37);
        CHECK(peak > 0.01f);
        std::printf("  %-12s note %3d: first non-zero frame %+d, > -60 dBFS %+d (%.2f ms), peak %.3f\n",
                    std::string(toString(i)).c_str(), note, any - (1024 + 37), audible - (1024 + 37),
                    (audible - (1024 + 37)) / 48.0, peak);
    }

    // Per-instrument volume: snare at 0 -> silent, kick unaffected.
    auto e = std::make_unique<AudioEngine>(kRate, false);
    e->installExercise(makeRtExercise(builtinExercise()));
    e->installKit(loadKitBundle(kit, kRate, true, &err));
    sendPadMap(*e, pads);
    e->send(Command::instrumentVolume(Instrument::Snare, 0.0));
    auto r = render(*e, 48000, [](int) { return 256; }, [&](int64_t done, int) {
        std::vector<MidiEvent> m;
        if (done == 2048) m.push_back({0, 3, {0x99, uint8_t(kit.notes[instrumentIndex(Instrument::Snare)]), 120}});
        if (done == 24064) m.push_back({0, 3, {0x99, uint8_t(kit.notes[instrumentIndex(Instrument::Kick)]), 120}});
        return m;
    });
    float snarePeak = 0, kickPeak = 0;
    for (int f = 0; f < 24064; ++f) snarePeak = std::max(snarePeak, std::fabs(r.left[size_t(f)]));
    for (int f = 24064; f < 48000; ++f) kickPeak = std::max(kickPeak, std::fabs(r.left[size_t(f)]));
    CHECK(snarePeak == 0.0f);
    CHECK(kickPeak > 0.01f);
    std::printf("  mixer: snare volume 0 -> peak %.4f, kick -> peak %.3f\n", snarePeak, kickPeak);

    // Velocity curve: gain 0.5 halves the velocity sent to the sampler.
    auto v = std::make_unique<AudioEngine>(kRate, true);
    v->installExercise(makeRtExercise(builtinExercise()));
    v->installKit(loadKitBundle(kit, kRate, true, &err));
    sendPadMap(*v, pads);
    v->send(Command::velocityCurve(Instrument::Kick, 0.5, 1.0));
    v->send(Command::velocityCurve(Instrument::Snare, 1.0, 2.0));
    auto vr = render(*v, 2048, [](int) { return 256; }, [&](int64_t done, int) {
        std::vector<MidiEvent> m;
        if (done == 0) {
            m.push_back({1, 3, {0x99, uint8_t(kit.notes[instrumentIndex(Instrument::Kick)]), 127}});
            m.push_back({2, 3, {0x99, uint8_t(kit.notes[instrumentIndex(Instrument::Snare)]), 64}});
        }
        return m;
    });
    std::vector<int> played;
    for (const auto& t : vr.trace)
        if (t.kind == TraceKind::Pad) played.push_back(t.velocity);
    CHECK(played.size() == 2 && played[0] == 64 && played[1] == 32);  // 127*0.5, 127*(64/127)^2
    std::printf("  velocity curves: kick gain 0.5 127 -> %d, snare exponent 2 64 -> %d\n",
                played.size() > 0 ? played[0] : -1, played.size() > 1 ? played[1] : -1);
}

int main() {
    testClickSchedule();
    testGrooveAndEnd();
    testTempoChange();
    testMidi();
    testPadsThroughSampler();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
