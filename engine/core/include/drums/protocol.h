// WebSocket protocol encode/decode (docs/PROTOCOL.md). CONTRACT FILE. Implemented in core/src.
#pragma once

#include "drums/exercise.h"
#include "drums/scoring.h"
#include "drums/types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace drums::proto {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kDefaultPort = 7878;

// ---- UI -> engine ----
struct Play {};
struct Stop {};
struct SetBpm { double bpm = 60; };
struct SetLoop { bool on = true; };
struct SetCountIn { int bars = 1; };
struct SetClick { std::optional<bool> on; std::optional<double> volume; std::optional<bool> subdivisions; };
struct SetGroove { std::optional<bool> on; std::optional<double> volume; };
struct LoadExercise { std::string path; };
struct LoadKit { std::string path; };
struct SetMixer { std::string channel; double volume = 1.0; };  // instrument name or "master"
struct SetLatencyOffset { double ms = 0; };
struct StartRecording {};
struct StopRecording {};
struct SetPadMap { int note = 0; Instrument instrument = Instrument::Unknown; };  // Unknown = unmap
struct GetState {};

using Command = std::variant<Play, Stop, SetBpm, SetLoop, SetCountIn, SetClick, SetGroove,
                             LoadExercise, LoadKit, SetMixer, SetLatencyOffset,
                             StartRecording, StopRecording, SetPadMap, GetState>;

// Validates types and ranges from PROTOCOL.md. On failure returns nullopt and fills *error.
std::optional<Command> parseCommand(std::string_view text, std::string* error = nullptr);

// ---- engine -> UI ----
struct KitInfo {
    std::string name;
    std::string path;
};

struct EngineState {
    bool playing = false;
    bool recording = false;
    double bpm = 60;
    bool loop = true;
    int countInBars = 1;
    bool clickOn = true;
    double clickVolume = 0.8;
    bool clickSubdivisions = true;
    bool grooveOn = false;
    double grooveVolume = 0.7;
    double latencyOffsetMs = 0;
    std::optional<Exercise> exercise;
    KitInfo kit;
    std::vector<KitInfo> kits;
    std::map<std::string, double> mixer;  // "master" + instrument names
    PadMap padMap;
};

std::string helloJson(int sampleRate, int bufferFrames, double outputLatencyMs);
std::string stateJson(const EngineState& state);
std::string positionJson(bool playing, double posTicks, int64_t loop, double tickInLoop, double timeMs);
std::string hitJson(const ScoredHit& hit);  // Grade::Free -> loop/tickInLoop null
std::string missedJson(const MissedNote& missed);
std::string loopSummaryJson(const LoopSummary& summary);
std::string recordingSavedJson(const std::string& path);
std::string errorJson(std::string_view message);

}  // namespace drums::proto
