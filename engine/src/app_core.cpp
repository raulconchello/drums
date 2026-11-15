// App thread linked with drums_core: WebSocket control server, scoring, MIDI recording.
#include "app.h"

#include "exercise_rt.h"
#include "kit.h"
#include "log.h"
#include "session.h"

#include "drums/control_server.h"
#include "drums/exercise.h"
#include "drums/midi_recorder.h"
#include "drums/protocol.h"
#include "drums/scoring.h"

#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace drums::engine {

namespace {
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
}  // namespace

struct App::Impl {
    AudioEngine& engine;
    AppConfig config;
    ControlServer server;
    Scorer scorer;
    MidiRecorder recorder;
    proto::EngineState state;
    PadMap padMap;
    std::string padMapPath;

    // UI commands arrive on the server thread; the app thread drains them.
    std::mutex inboxMutex;
    std::condition_variable inboxCv;
    std::deque<std::string> inbox;

    // Latest state JSON for clients that connect (read on the server thread).
    std::mutex stateMutex;
    std::string stateCache;

    bool stopPositionPending = false;  // send one "position" with playing=false after a stop
    std::chrono::steady_clock::time_point lastPosition{};
    int64_t recordOriginFrame = 0;

    Impl(AudioEngine& e, AppConfig c)
        : engine(e), config(std::move(c)), server(uint16_t(config.options.port)) {}

    // ---------------------------------------------------------------- helpers
    const Exercise& exercise() const { return *state.exercise; }
    double sr() const { return double(config.sampleRate); }

    void broadcastState() {
        std::string json = proto::stateJson(state);
        {
            std::lock_guard lock(stateMutex);
            stateCache = json;
        }
        server.broadcast(json);
    }

    void error(const std::string& message) {
        logWarn("%s", message.c_str());
        server.broadcast(proto::errorJson(message));
    }

    double recordTimeMs(int64_t frame) const { return double(frame - recordOriginFrame) * 1000.0 / sr(); }

    // Audible (latency-compensated) position, the domain of hits and scoring.
    static double audiblePos(const TransportSnapshot& s) {
        return s.posTicks - (s.msPerTick > 0 ? s.compensationMs / s.msPerTick : 0.0);
    }

    void sendPosition(const TransportSnapshot& s, bool playing) {
        const double pos = audiblePos(s);
        const double len = double(exercise().lengthTicks());
        const auto loop = int64_t(std::floor(pos / len));
        const double timeMs = double(s.frame - s.playStartFrame) * 1000.0 / sr() - s.compensationMs;
        server.broadcast(proto::positionJson(playing, pos, loop, pos - double(loop) * len, timeMs));
    }

    void scoreUpTo(double pos, double msPerTick) {
        for (const auto& m : scorer.advance(pos, msPerTick)) server.broadcast(proto::missedJson(m));
        for (const auto& s : scorer.takeLoopSummaries()) server.broadcast(proto::loopSummaryJson(s));
    }

    // ---------------------------------------------------------------- loading
    bool setExercise(Exercise ex, std::string* err) {
        if (ex.lengthTicks() <= 0 || ex.beatTicks <= 0) {
            if (err) *err = "exercise has no length";
            return false;
        }
        scorer.setExercise(ex);
        scorer.setLooping(state.loop);
        engine.send(Command::exercise(makeRtExercise(ex)));
        state.bpm = std::clamp(ex.defaultBpm, 20.0, 300.0);
        engine.send(Command::bpm(state.bpm));
        state.exercise = std::move(ex);
        return true;
    }

    bool loadKit(const std::string& path, bool install, std::string* err) {
        try {
            const KitConfig kit = loadKitConfig(resolvePath(config.root, path));
            auto bundle = loadKitBundle(kit, sr(), false, err);
            if (!bundle) return false;
            if (install) engine.installKit(std::move(bundle));
            else engine.send(Command::kit(std::move(bundle)));
            state.kit = {kit.name, displayPath(config.root, kit.jsonPath)};
            state.kits = toKitInfos(scanKits(config.root));
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return false;
        }
    }

    static std::vector<proto::KitInfo> toKitInfos(const std::vector<KitListEntry>& list) {
        std::vector<proto::KitInfo> out;
        for (const auto& k : list) out.push_back({k.name, k.path});
        return out;
    }

    std::string recordingPath() const {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char name[64];
        std::strftime(name, sizeof name, "%Y-%m-%dT%H-%M-%S.mid", &tm);
        return (fs::path(config.root) / "recordings" / name).string();
    }

    void stopRecording() {
        if (!state.recording) {
            error("stopRecording: not recording");
            return;
        }
        const std::string path = recordingPath();
        state.recording = false;
        if (recorder.stopAndSave(path)) {
            logInfo("recording saved: %s", path.c_str());
            server.broadcast(proto::recordingSavedJson(path));
        } else {
            error("could not save the recording to " + path);
        }
    }

    // ---------------------------------------------------------------- commands
    void apply(const proto::Command& command) {
        std::visit(
            Overloaded{
                [&](const proto::Play&) {
                    if (state.playing) return;
                    scorer.reset();
                    scorer.setLooping(state.loop);
                    engine.send(Command::play());
                    state.playing = true;
                    stopPositionPending = false;
                },
                [&](const proto::Stop&) {
                    if (!state.playing) return;
                    engine.send(Command::stop());
                    state.playing = false;
                    stopPositionPending = true;
                },
                [&](const proto::SetBpm& c) {
                    state.bpm = std::clamp(c.bpm, 20.0, 300.0);
                    engine.send(Command::bpm(state.bpm));
                },
                [&](const proto::SetLoop& c) {
                    state.loop = c.on;
                    scorer.setLooping(c.on);
                    engine.send(Command::loop(c.on));
                },
                [&](const proto::SetCountIn& c) {
                    state.countInBars = std::clamp(c.bars, 0, 2);
                    engine.send(Command::countIn(state.countInBars));
                },
                [&](const proto::SetClick& c) {
                    if (c.on) state.clickOn = *c.on;
                    if (c.volume) state.clickVolume = std::clamp(*c.volume, 0.0, 1.0);
                    if (c.subdivisions) state.clickSubdivisions = *c.subdivisions;
                    engine.send(Command::click(c.on, c.volume, c.subdivisions));
                },
                [&](const proto::SetGroove& c) {
                    if (c.on) state.grooveOn = *c.on;
                    if (c.volume) state.grooveVolume = std::clamp(*c.volume, 0.0, 1.0);
                    engine.send(Command::groove(c.on, c.volume));
                },
                [&](const proto::LoadExercise& c) {
                    try {
                        Exercise ex = loadExercise(resolvePath(config.root, c.path));
                        if (state.playing) {
                            engine.send(Command::stop());
                            state.playing = false;
                            stopPositionPending = true;
                        }
                        std::string err;
                        if (!setExercise(std::move(ex), &err)) error("loadExercise: " + err);
                    } catch (const std::exception& e) {
                        error(std::string("loadExercise: ") + e.what());
                    }
                },
                [&](const proto::LoadKit& c) {
                    std::string err;
                    if (!loadKit(c.path, false, &err)) error("loadKit: " + err);
                },
                [&](const proto::SetMixer& c) {
                    const double v = std::clamp(c.volume, 0.0, 1.5);
                    if (c.channel == "master") {
                        engine.send(Command::masterVolume(v));
                    } else {
                        const Instrument i = instrumentFromString(c.channel);
                        if (i == Instrument::Unknown) return error("setMixer: unknown channel " + c.channel);
                        engine.send(Command::instrumentVolume(i, v));
                    }
                    state.mixer[c.channel] = v;
                },
                [&](const proto::SetLatencyOffset& c) {
                    state.latencyOffsetMs = std::clamp(c.ms, -50.0, 100.0);
                    engine.send(Command::latencyOffset(state.latencyOffsetMs));
                },
                [&](const proto::StartRecording&) {
                    const Exercise& ex = exercise();
                    recorder.start(state.bpm * ex.beatTicks / ex.ppq, ex.timeSignature);
                    // Time zero: now, or (while playing) the audible start of the current bar so
                    // the file's bar lines match the exercise.
                    const TransportSnapshot s = engine.snapshot();
                    recordOriginFrame = s.frame;
                    if (s.playing && s.msPerTick > 0) {
                        const double pos = audiblePos(s);
                        const double bar = double(ex.barTicks());
                        const double sinceBar = pos - std::floor(pos / bar) * bar;
                        recordOriginFrame = s.frame - int64_t(std::llround(sinceBar * s.msPerTick * sr() / 1000.0));
                    }
                    state.recording = true;
                },
                [&](const proto::StopRecording&) { stopRecording(); },
                [&](const proto::SetPadMap& c) {
                    padMap.byNote[size_t(c.note)] = c.instrument;
                    state.padMap = padMap;
                    engine.send(Command::padNote(c.note, c.instrument));
                    if (!savePadMap(padMap, padMapPath)) error("could not save " + padMapPath);
                },
                [&](const proto::GetState&) {},
            },
            command);
    }

    // ---------------------------------------------------------------- engine events
    void onHit(const Event& ev) {
        ScoredHit hit;
        if (ev.playing) {
            hit = scorer.onHit(ev.instrument, ev.note, ev.velocity, ev.posTicks, ev.msPerTick);
        } else {
            hit.instrument = ev.instrument;
            hit.note = ev.note;
            hit.velocity = ev.velocity;
            hit.grade = Grade::Free;
        }
        server.broadcast(proto::hitJson(hit));
        if (recorder.isRecording()) recorder.noteOn(ev.note, ev.velocity, recordTimeMs(ev.frame));
        if (config.options.logHits) {
            logInfo("hit note %d vel %d %s: %s%s loop %lld tick %.1f error %s", ev.note, ev.velocity,
                    std::string(toString(ev.instrument)).c_str(), std::string(toString(hit.grade)).c_str(),
                    ev.playing ? "" : " (stopped)", static_cast<long long>(hit.loop), hit.tickInLoop,
                    hit.errorMs ? std::to_string(*hit.errorMs).c_str() : "-");
        }
    }

    void drainEngine() {
        Event ev;
        while (engine.poll(ev)) {
            switch (ev.type) {
                case EventType::Hit:
                    onHit(ev);
                    break;
                case EventType::NoteOff:
                    if (recorder.isRecording()) recorder.noteOff(ev.note, recordTimeMs(ev.frame));
                    break;
                case EventType::Control:
                    if (recorder.isRecording()) recorder.controlChange(ev.controller, ev.value, recordTimeMs(ev.frame));
                    break;
                case EventType::Stopped:
                    // Exercise finished (no loop): close the remaining windows, report, then stop.
                    if (state.playing) {
                        scoreUpTo(ev.posTicks + 1000.0 / ev.msPerTick, ev.msPerTick);
                        state.playing = false;
                        stopPositionPending = true;
                        broadcastState();
                    }
                    break;
                case EventType::Garbage:
                    AudioEngine::dispose(ev);
                    break;
            }
        }
    }
};

App::App(AudioEngine& engine, AppConfig config) : impl_(std::make_unique<Impl>(engine, std::move(config))) {}
App::~App() = default;

bool App::hasCore() { return true; }

bool App::prepare(std::string* error) {
    auto& d = *impl_;
    const Options& o = d.config.options;

    d.padMapPath = resolvePath(d.config.root, o.padMap.empty() ? "kits/pad-map.json" : o.padMap);
    try {
        d.padMap = loadPadMap(d.padMapPath);
        logInfo("pad map: %s", d.padMapPath.c_str());
    } catch (const std::exception& e) {
        logWarn("pad map %s: %s (pads unmapped until set from the UI)", d.padMapPath.c_str(), e.what());
    }
    d.state.padMap = d.padMap;
    sendPadMap(d.engine, d.padMap);

    // Exercise: --exercise, else exercises/groove-12-8.json, else the built-in copy.
    Exercise ex;
    try {
        ex = loadExercise(resolvePath(d.config.root, o.exercise.empty() ? "exercises/groove-12-8.json" : o.exercise));
    } catch (const std::exception& e) {
        if (!o.exercise.empty()) {
            if (error) *error = std::string("exercise: ") + e.what();
            return false;
        }
        logWarn("%s; using the built-in 12/8 groove", e.what());
        ex = builtinExercise();
    }
    d.engine.installExercise(makeRtExercise(ex));
    d.scorer.setExercise(ex);
    logInfo("exercise: %s (%d/%d, %d bar(s), %zu notes)", ex.title.c_str(), ex.timeSignature.numerator,
            ex.timeSignature.denominator, ex.bars, ex.notes.size());

    d.state.kits = Impl::toKitInfos(scanKits(d.config.root));
    const std::string kitPath = o.kit.empty() ? defaultKitPath(d.config.root) : o.kit;
    if (kitPath.empty()) {
        logWarn("no kit found (run kits/fetch-kits.sh): pads and groove are silent");
    } else {
        std::string err;
        if (!d.loadKit(kitPath, true, &err)) {
            if (error) *error = "kit: " + err;
            return false;
        }
    }

    const InitialSettings s = resolveSettings(o, ex.defaultBpm, false);
    sendSettings(d.engine, s);
    d.state.bpm = s.bpm;
    d.state.countInBars = s.countInBars;
    d.state.loop = s.loop;
    d.state.clickOn = s.clickOn;
    d.state.clickVolume = s.clickVolume;
    d.state.clickSubdivisions = s.clickSubdivisions;
    d.state.grooveOn = s.grooveOn;
    d.state.grooveVolume = s.grooveVolume;
    d.state.latencyOffsetMs = 0;
    d.state.mixer["master"] = s.master;
    for (Instrument i : kInstruments) d.state.mixer[std::string(toString(i))] = 1.0;
    d.scorer.setLooping(s.loop);
    d.state.exercise = std::move(ex);
    if (o.play) d.apply(proto::Play{});
    return true;
}

bool App::startServer(std::string* error) {
    auto& d = *impl_;
    d.server.onCommand([&d](const std::string& text) {
        {
            std::lock_guard lock(d.inboxMutex);
            d.inbox.push_back(text);
        }
        d.inboxCv.notify_one();
    });
    d.server.onConnect([&d]() {
        std::vector<std::string> messages;
        messages.push_back(proto::helloJson(d.config.sampleRate, d.config.bufferFrames(), d.config.outputLatencyMs()));
        std::lock_guard lock(d.stateMutex);
        messages.push_back(d.stateCache);
        return messages;
    });
    {
        std::lock_guard lock(d.stateMutex);
        d.stateCache = proto::stateJson(d.state);
    }
    std::string err;
    if (!d.server.start(&err)) {
        if (error) *error = "WebSocket server on port " + std::to_string(d.config.options.port) + ": " + err;
        return false;
    }
    logInfo("control server: ws://127.0.0.1:%d/", d.config.options.port);
    return true;
}

void App::runOnce(std::chrono::milliseconds wait) {
    auto& d = *impl_;
    std::deque<std::string> commands;
    {
        std::unique_lock lock(d.inboxMutex);
        d.inboxCv.wait_for(lock, wait, [&d] { return !d.inbox.empty(); });
        commands.swap(d.inbox);
    }

    // Snapshot first, then drain: every hit up to the snapshot's frame is then already scored when
    // advance() closes windows at that position.
    const TransportSnapshot snap = d.engine.snapshot();
    d.drainEngine();

    for (const auto& text : commands) {
        std::string err;
        const auto command = proto::parseCommand(text, &err);
        if (!command) {
            d.error(err.empty() ? "bad command" : err);
            continue;
        }
        d.apply(*command);
        d.broadcastState();
    }

    const auto now = std::chrono::steady_clock::now();
    if (snap.playing && d.state.playing) {
        if (now - d.lastPosition >= std::chrono::milliseconds(16)) {
            d.lastPosition = now;
            d.sendPosition(snap, true);
            d.scoreUpTo(Impl::audiblePos(snap), snap.msPerTick);
        }
    } else if (d.stopPositionPending && !snap.playing) {
        d.stopPositionPending = false;
        d.sendPosition(snap, false);
    }
}

void App::shutdown() {
    auto& d = *impl_;
    if (d.state.recording) d.stopRecording();
    d.server.stop();
}

}  // namespace drums::engine
