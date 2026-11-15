#include "session.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace drums::engine {

std::unique_ptr<KitBundle> loadKitBundle(const KitConfig& kit, double sampleRate, bool freewheel, std::string* error) {
    auto bundle = std::make_unique<KitBundle>();
    bundle->player = DrumSampler::create(kit, sampleRate, kChunkFrames, freewheel, error);
    if (!bundle->player) return nullptr;
    bundle->groove = DrumSampler::create(kit, sampleRate, kChunkFrames, freewheel, error);
    if (!bundle->groove) return nullptr;
    return bundle;
}

PadMap readPadMapJson(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    const auto j = nlohmann::json::parse(in);
    PadMap map;
    if (j.contains("hihatPedalCC") && j["hihatPedalCC"].is_number_integer())
        map.hihatPedalCC = j["hihatPedalCC"].get<int>();
    if (j.contains("notes") && j["notes"].is_object()) {
        for (const auto& [key, value] : j["notes"].items()) {
            const int note = std::stoi(key);
            if (note < 0 || note > 127 || !value.is_string()) continue;
            map.byNote[size_t(note)] = instrumentFromString(value.get<std::string>());
        }
    }
    return map;
}

void sendPadMap(AudioEngine& engine, const PadMap& map) {
    for (int note = 0; note < 128; ++note) engine.send(Command::padNote(note, map[note]));
    engine.send(Command::hihatPedalCC(map.hihatPedalCC));
}

InitialSettings resolveSettings(const Options& o, double exerciseDefaultBpm, bool grooveDefault) {
    InitialSettings s;
    s.bpm = o.bpm.value_or(exerciseDefaultBpm);
    s.countInBars = o.countIn.value_or(1);
    s.loop = o.loop.value_or(true);
    s.clickOn = o.click.value_or(true);
    s.clickSubdivisions = o.subdivisions.value_or(true);
    s.grooveOn = o.groove.value_or(grooveDefault);
    return s;
}

void sendSettings(AudioEngine& engine, const InitialSettings& s) {
    engine.send(Command::bpm(s.bpm));
    engine.send(Command::countIn(s.countInBars));
    engine.send(Command::loop(s.loop));
    engine.send(Command::click(s.clickOn, s.clickVolume, s.clickSubdivisions));
    engine.send(Command::groove(s.grooveOn, s.grooveVolume));
    engine.send(Command::masterVolume(s.master));
}

}  // namespace drums::engine
