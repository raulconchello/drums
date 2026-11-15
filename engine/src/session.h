// Non-real-time helpers shared by the app thread and the offline renderer.
#pragma once

#include "audio_engine.h"
#include "kit.h"
#include "options.h"

#include "drums/types.h"

#include <memory>
#include <string>

namespace drums::engine {

// Loads the kit twice (pads + groove). Returns nullptr and fills *error on failure.
std::unique_ptr<KitBundle> loadKitBundle(const KitConfig& kit, double sampleRate, bool freewheel, std::string* error);

// kits/pad-map.json reader used when the core library is not linked (throws std::runtime_error).
PadMap readPadMapJson(const std::string& path);

// Queues the whole pad map (128 notes + hi-hat pedal CC) for the audio callback.
void sendPadMap(AudioEngine& engine, const PadMap& map);

// Transport/mixer settings resolved from defaults + command line.
struct InitialSettings {
    double bpm = 60;
    int countInBars = 1;
    bool loop = true;
    bool clickOn = true;
    double clickVolume = 0.8;
    bool clickSubdivisions = true;
    bool grooveOn = false;
    double grooveVolume = 0.7;
    double master = 0.8;
};
InitialSettings resolveSettings(const Options& options, double exerciseDefaultBpm, bool grooveDefault);
void sendSettings(AudioEngine& engine, const InitialSettings& settings);

}  // namespace drums::engine
