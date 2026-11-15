// drums-engine: real-time audio engine of the drum practice app.
//   live:    JACK (PipeWire) client + app thread (WebSocket UI server, scoring, recording)
//   offline: --offline-render out.wav renders click + groove without JACK (tests)
#include "app.h"
#include "audio_engine.h"
#include "exercise_rt.h"
#include "jack_backend.h"
#include "kit.h"
#include "log.h"
#include "options.h"
#include "session.h"
#include "wav_writer.h"

#ifdef DRUMS_WITH_CORE
#include "drums/exercise.h"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace drums;
using namespace drums::engine;

namespace {

std::atomic<bool> gQuit{false};

void onSignal(int) { gQuit.store(true); }

void installSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

const char* traceName(TraceKind k) {
    switch (k) {
        case TraceKind::Accent: return "accent";
        case TraceKind::Beat: return "beat";
        case TraceKind::Subdivision: return "sub";
        case TraceKind::Groove: return "groove";
        case TraceKind::Pad: return "pad";
    }
    return "?";
}

int runOffline(const Options& o, const std::string& root) {
    constexpr int kRate = 48000;
    AudioEngine engine(kRate, o.trace);

    Exercise ex = builtinExercise();
#ifdef DRUMS_WITH_CORE
    try {
        ex = loadExercise(resolvePath(root, o.exercise.empty() ? "exercises/groove-12-8.json" : o.exercise));
    } catch (const std::exception& e) {
        if (!o.exercise.empty()) {
            logError("exercise: %s", e.what());
            return 1;
        }
        logWarn("%s; using the built-in 12/8 groove", e.what());
    }
#else
    if (!o.exercise.empty()) logWarn("--exercise ignored: built without drums_core (using the built-in 12/8 groove)");
#endif
    engine.installExercise(makeRtExercise(ex));

    const InitialSettings s = resolveSettings(o, ex.defaultBpm, /*grooveDefault=*/true);
    const std::string kitPath = o.kit.empty() ? defaultKitPath(root) : resolvePath(root, o.kit);
    if (s.grooveOn) {
        if (kitPath.empty()) {
            logWarn("no kit found: the groove is silent (run kits/fetch-kits.sh)");
        } else {
            try {
                std::string err;
                auto bundle = loadKitBundle(loadKitConfig(kitPath), kRate, /*freewheel=*/true, &err);
                if (!bundle) throw std::runtime_error(err);
                engine.installKit(std::move(bundle));
            } catch (const std::exception& e) {
                logError("kit: %s", e.what());
                return 1;
            }
        }
    }
    sendSettings(engine, s);
    engine.send(Command::play());

    const int64_t total = int64_t(std::llround(o.seconds * kRate));
    std::vector<float> left(static_cast<size_t>(total)), right(static_cast<size_t>(total));
    const int block = o.buffer;
    int64_t done = 0;
    bool stopped = false;
    while (done < total) {
        const int n = int(std::min<int64_t>(block, total - done));
        engine.process(n, nullptr, 0, left.data() + done, right.data() + done);
        done += n;
        Event ev;
        while (engine.poll(ev)) {
            if (ev.type == EventType::Stopped && !stopped) {
                stopped = true;
                logInfo("transport stopped at frame %lld (end of the exercise)", static_cast<long long>(ev.frame));
            }
            AudioEngine::dispose(ev);
        }
    }
    if (o.trace) {
        TraceEvent t;
        while (engine.pollTrace(t)) {
            std::printf("%lld %.3f %s", static_cast<long long>(t.frame), t.tick, traceName(t.kind));
            if (t.kind == TraceKind::Groove || t.kind == TraceKind::Pad)
                std::printf(" %s %d %d", std::string(toString(t.instrument)).c_str(), t.note, t.velocity);
            std::printf("\n");
        }
    }
    float peak = 0;
    for (size_t i = 0; i < left.size(); ++i) peak = std::max({peak, std::fabs(left[i]), std::fabs(right[i])});
    if (!writeWavFloatStereo(o.offlineRender, kRate, left, right)) {
        logError("cannot write %s", o.offlineRender.c_str());
        return 1;
    }
    logInfo("offline render: %s, %lld frames @ %d Hz (%.2f s), %d bpm, block %d, click %s%s, groove %s, peak %.3f",
            o.offlineRender.c_str(), static_cast<long long>(total), kRate, double(total) / kRate, int(s.bpm), block,
            s.clickOn ? "on" : "off", s.clickSubdivisions ? "+subdivisions" : "", s.grooveOn ? "on" : "off", peak);
    return 0;
}

int runLive(const Options& o, const std::string& root) {
    JackBackend backend;
    JackBackend::Config jc;
    jc.clientName = o.clientName;
    jc.bufferFrames = o.buffer;
    jc.midiMatch = o.midiMatch;
    jc.audioMatch = o.audioMatch;
    jc.autoconnect = o.autoconnect;
    std::string err;
    if (!backend.open(jc, &err)) {
        logError("%s", err.c_str());
        return 1;
    }

    AudioEngine engine(backend.sampleRate());
    AppConfig ac;
    ac.options = o;
    ac.root = root;
    ac.sampleRate = backend.sampleRate();
    ac.bufferFrames = [&backend] { return backend.bufferSize(); };
    ac.outputLatencyMs = [&backend] { return backend.outputLatencyMs(); };
    App app(engine, ac);
    if (!app.prepare(&err)) {
        logError("%s", err.c_str());
        return 1;
    }
    if (!app.startServer(&err)) {
        logError("%s", err.c_str());
        return 1;
    }
    if (!backend.start(&engine, &err)) {
        logError("%s", err.c_str());
        app.shutdown();
        return 1;
    }
    logInfo("running (Ctrl+C to quit)");

    auto lastPoll = std::chrono::steady_clock::now();
    bool serverAlive = true;
    while (!gQuit.load() && serverAlive) {
        app.runOnce(std::chrono::milliseconds(8));
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPoll >= std::chrono::milliseconds(100)) {
            lastPoll = now;
            serverAlive = backend.poll();
        }
    }
    logInfo("shutting down");
    backend.stop();   // the audio callback is stopped after this
    app.runOnce(std::chrono::milliseconds(0));  // last hits / garbage
    app.shutdown();
    if (engine.droppedEvents() > 0)
        logWarn("%llu engine events were dropped (queue full)", static_cast<unsigned long long>(engine.droppedEvents()));
    return serverAlive ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parseOptions(argc, argv);
    const std::string root = o.root.empty() ? detectRoot(argv[0]) : fs::absolute(o.root).lexically_normal().string();
    if (o.listPorts) return JackBackend::listPorts(o.midiMatch, o.audioMatch);
    installSignalHandlers();
    logInfo("root: %s (core library: %s)", root.c_str(), App::hasCore() ? "linked" : "not linked");
    if (!o.offlineRender.empty()) return runOffline(o, root);
    return runLive(o, root);
}
