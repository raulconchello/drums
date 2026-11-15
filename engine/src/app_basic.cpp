// App thread without the core library: no WebSocket, scoring or recording. Plays the pads, the
// metronome and the built-in 12/8 groove; useful for audio tests while drums_core is unavailable.
#include "app.h"

#include "exercise_rt.h"
#include "kit.h"
#include "log.h"
#include "session.h"

#include <thread>

namespace drums::engine {

struct App::Impl {
    AudioEngine& engine;
    AppConfig config;
    uint64_t hits = 0;

    Impl(AudioEngine& e, AppConfig c) : engine(e), config(std::move(c)) {}
};

App::App(AudioEngine& engine, AppConfig config) : impl_(std::make_unique<Impl>(engine, std::move(config))) {}
App::~App() = default;

bool App::hasCore() { return false; }

bool App::prepare(std::string* error) {
    auto& d = *impl_;
    const Options& o = d.config.options;
    const std::string padPath = resolvePath(d.config.root, o.padMap.empty() ? "kits/pad-map.json" : o.padMap);
    try {
        sendPadMap(d.engine, readPadMapJson(padPath));
        logInfo("pad map: %s", padPath.c_str());
    } catch (const std::exception& e) {
        logWarn("pad map %s: %s (pads unmapped)", padPath.c_str(), e.what());
    }

    if (!o.exercise.empty()) logWarn("--exercise ignored: built without drums_core (using the built-in 12/8 groove)");
    const Exercise ex = builtinExercise();
    d.engine.installExercise(makeRtExercise(ex));

    const std::string kitPath = o.kit.empty() ? defaultKitPath(d.config.root) : resolvePath(d.config.root, o.kit);
    if (kitPath.empty()) {
        logWarn("no kit found (run kits/fetch-kits.sh): pads and groove are silent");
    } else {
        try {
            const KitConfig kit = loadKitConfig(kitPath);
            std::string err;
            auto bundle = loadKitBundle(kit, d.config.sampleRate, false, &err);
            if (!bundle) throw std::runtime_error(err);
            d.engine.installKit(std::move(bundle));
        } catch (const std::exception& e) {
            if (error) *error = std::string("kit: ") + e.what();
            return false;
        }
    }
    sendSettings(d.engine, resolveSettings(o, ex.defaultBpm, false));
    if (o.play) d.engine.send(Command::play());
    return true;
}

bool App::startServer(std::string*) {
    logWarn("built without drums_core: no WebSocket control server, scoring or recording");
    return true;
}

void App::runOnce(std::chrono::milliseconds wait) {
    auto& d = *impl_;
    std::this_thread::sleep_for(wait);
    Event ev;
    while (d.engine.poll(ev)) {
        switch (ev.type) {
            case EventType::Hit:
                ++d.hits;
                if (d.config.options.logHits) {
                    logInfo("hit #%llu note %d vel %d -> %s  frame %lld  %s posTicks %.2f",
                            static_cast<unsigned long long>(d.hits), ev.note, ev.velocity,
                            std::string(toString(ev.instrument)).c_str(), static_cast<long long>(ev.frame),
                            ev.playing ? "playing" : "stopped(free)", ev.posTicks);
                }
                break;
            case EventType::NoteOff:
                if (d.config.options.logHits)
                    logInfo("note-off %d frame %lld", ev.note, static_cast<long long>(ev.frame));
                break;
            case EventType::Control:
                if (d.config.options.logHits)
                    logInfo("cc %d = %d frame %lld", ev.controller, ev.value, static_cast<long long>(ev.frame));
                break;
            case EventType::Stopped:
                logInfo("transport stopped at the end of the exercise");
                break;
            case EventType::Garbage:
                AudioEngine::dispose(ev);
                break;
        }
    }
}

void App::shutdown() {}

}  // namespace drums::engine
