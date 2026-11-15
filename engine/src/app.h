// The engine's non-real-time "app thread": consumes hits from the audio callback, scores and
// records them, serves the UI over WebSocket and turns UI commands into engine commands.
// Two implementations: app_core.cpp (linked with drums_core) and app_basic.cpp (audio only).
#pragma once

#include "audio_engine.h"
#include "options.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace drums::engine {

struct AppConfig {
    Options options;
    std::string root;
    int sampleRate = 48000;
    std::function<int()> bufferFrames;       // current audio period
    std::function<double()> outputLatencyMs; // current output port latency
};

class App {
public:
    App(AudioEngine& engine, AppConfig config);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Loads pad map, exercise and kit and installs them in the engine (call before audio starts),
    // then queues the initial settings.
    bool prepare(std::string* error);
    // Starts the WebSocket control server (no-op in the audio-only build).
    bool startServer(std::string* error);
    // One app-thread iteration: waits up to `wait` for UI commands, then handles commands, engine
    // events (hits, recording, garbage), position/missed/loop-summary messages.
    void runOnce(std::chrono::milliseconds wait);
    // Saves a running recording, stops the server.
    void shutdown();

    static bool hasCore();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace drums::engine
