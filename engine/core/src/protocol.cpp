#include "drums/protocol.h"

#include "json_util.h"

#include <cmath>
#include <sstream>

namespace drums::proto {
namespace {

using json = nlohmann::json;
using detail::ojson;
using detail::roundedNumber;
using detail::plainNumber;

// Output precision: ms to 0.1, ticks to 0.01.
ojson ms(double v) { return roundedNumber(v, 10); }
ojson ticks(double v) { return roundedNumber(v, 100); }

// ---- command parsing ----

struct ParseError {
    std::string message;
};

[[noreturn]] void reject(const std::string& type, const std::string& message) {
    throw ParseError{type + ": " + message};
}

std::string fmt(double v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

const json* find(const json& obj, const char* key) {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

double requireNumber(const json& obj, const std::string& type, const char* key, double min, double max) {
    const json* v = find(obj, key);
    if (!v) reject(type, std::string("missing \"") + key + "\"");
    if (!v->is_number() || !(v->get<double>() >= min && v->get<double>() <= max))
        reject(type, std::string("\"") + key + "\" must be a number between " + fmt(min) + " and " + fmt(max));
    return v->get<double>();
}

std::optional<double> optionalNumber(const json& obj, const std::string& type, const char* key, double min,
                                     double max) {
    if (!find(obj, key)) return std::nullopt;
    return requireNumber(obj, type, key, min, max);
}

int requireInt(const json& obj, const std::string& type, const char* key, int min, int max) {
    const json* v = find(obj, key);
    if (!v) reject(type, std::string("missing \"") + key + "\"");
    if (v->is_number()) {
        const double d = v->get<double>();
        if (d == std::floor(d) && d >= min && d <= max) return static_cast<int>(d);
    }
    reject(type, std::string("\"") + key + "\" must be an integer between " + std::to_string(min) + " and " +
                     std::to_string(max));
}

bool requireBool(const json& obj, const std::string& type, const char* key) {
    const json* v = find(obj, key);
    if (!v) reject(type, std::string("missing \"") + key + "\"");
    if (!v->is_boolean()) reject(type, std::string("\"") + key + "\" must be true or false");
    return v->get<bool>();
}

std::optional<bool> optionalBool(const json& obj, const std::string& type, const char* key) {
    if (!find(obj, key)) return std::nullopt;
    return requireBool(obj, type, key);
}

std::string requirePath(const json& obj, const std::string& type) {
    const json* v = find(obj, "path");
    if (!v) reject(type, "missing \"path\"");
    if (!v->is_string() || v->get<std::string>().empty()) reject(type, "\"path\" must be a non-empty string");
    return v->get<std::string>();
}

Command parse(const json& j) {
    if (!j.is_object()) throw ParseError{"a command must be a JSON object"};
    const json* t = find(j, "type");
    if (!t || !t->is_string()) throw ParseError{"a command needs a string \"type\""};
    const std::string type = t->get<std::string>();

    if (type == "play") return Play{};
    if (type == "stop") return Stop{};
    if (type == "startRecording") return StartRecording{};
    if (type == "stopRecording") return StopRecording{};
    if (type == "getState") return GetState{};
    if (type == "setBpm") return SetBpm{requireNumber(j, type, "bpm", 20, 300)};
    if (type == "setLoop") return SetLoop{requireBool(j, type, "on")};
    if (type == "setCountIn") return SetCountIn{requireInt(j, type, "bars", 0, 2)};
    if (type == "setClick") {
        SetClick c{optionalBool(j, type, "on"), optionalNumber(j, type, "volume", 0, 1),
                   optionalBool(j, type, "subdivisions")};
        if (!c.on && !c.volume && !c.subdivisions)
            reject(type, "needs at least one of \"on\", \"volume\", \"subdivisions\"");
        return c;
    }
    if (type == "setGroove") {
        SetGroove g{optionalBool(j, type, "on"), optionalNumber(j, type, "volume", 0, 1)};
        if (!g.on && !g.volume) reject(type, "needs at least one of \"on\", \"volume\"");
        return g;
    }
    if (type == "loadExercise") return LoadExercise{requirePath(j, type)};
    if (type == "loadKit") return LoadKit{requirePath(j, type)};
    if (type == "setMixer") {
        const json* ch = find(j, "channel");
        if (!ch) reject(type, "missing \"channel\"");
        if (!ch->is_string()) reject(type, "\"channel\" must be \"master\" or an instrument name");
        const std::string channel = ch->get<std::string>();
        if (channel != "master" && instrumentFromString(channel) == Instrument::Unknown)
            reject(type, "unknown channel \"" + channel + "\" (use \"master\" or an instrument name)");
        return SetMixer{channel, requireNumber(j, type, "volume", 0, 1.5)};
    }
    if (type == "setLatencyOffset") return SetLatencyOffset{requireNumber(j, type, "ms", -50, 100)};
    if (type == "setPadMap") {
        SetPadMap p;
        p.note = requireInt(j, type, "note", 0, 127);
        const json* inst = find(j, "instrument");
        if (!inst) reject(type, "missing \"instrument\" (an instrument name, or null to unmap)");
        if (inst->is_null()) {
            p.instrument = Instrument::Unknown;
        } else if (inst->is_string()) {
            p.instrument = instrumentFromString(inst->get<std::string>());
            if (p.instrument == Instrument::Unknown)
                reject(type, "unknown instrument \"" + inst->get<std::string>() + "\"");
        } else {
            reject(type, "\"instrument\" must be an instrument name or null");
        }
        return p;
    }
    throw ParseError{"unknown command type \"" + type + "\""};
}

ojson statsJson(const InstrumentStats& s) {
    return ojson{{"hits", s.hits},
                 {"missed", s.missed},
                 {"extra", s.extra},
                 {"meanErrorMs", ms(s.meanErrorMs)},
                 {"stdDevMs", ms(s.stdDevMs)}};
}

ojson kitJson(const KitInfo& k) { return ojson{{"name", k.name}, {"path", k.path}}; }

}  // namespace

std::optional<Command> parseCommand(std::string_view text, std::string* error) {
    try {
        json j = json::parse(text.begin(), text.end());
        return parse(j);
    } catch (const ParseError& e) {
        if (error) *error = e.message;
    } catch (const json::parse_error&) {
        if (error) *error = "invalid JSON";
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid command: ") + e.what();
    }
    return std::nullopt;
}

std::string helloJson(int sampleRate, int bufferFrames, double outputLatencyMs) {
    return detail::dump(ojson{{"type", "hello"},
                              {"protocol", kProtocolVersion},
                              {"sampleRate", sampleRate},
                              {"bufferFrames", bufferFrames},
                              {"outputLatencyMs", ms(outputLatencyMs)}});
}

std::string stateJson(const EngineState& st) {
    ojson kits = ojson::array();
    for (const KitInfo& k : st.kits) kits.push_back(kitJson(k));
    ojson mixer = ojson::object();
    for (const auto& [channel, volume] : st.mixer) mixer[channel] = plainNumber(volume);
    ojson padMap = ojson::object();
    for (int n = 0; n < 128; ++n)
        if (st.padMap.byNote[size_t(n)] != Instrument::Unknown)
            padMap[std::to_string(n)] = toString(st.padMap.byNote[size_t(n)]);

    return detail::dump(ojson{
        {"type", "state"},
        {"playing", st.playing},
        {"recording", st.recording},
        {"bpm", plainNumber(st.bpm)},
        {"loop", st.loop},
        {"countInBars", st.countInBars},
        {"click", {{"on", st.clickOn}, {"volume", plainNumber(st.clickVolume)}, {"subdivisions", st.clickSubdivisions}}},
        {"groove", {{"on", st.grooveOn}, {"volume", plainNumber(st.grooveVolume)}}},
        {"latencyOffsetMs", plainNumber(st.latencyOffsetMs)},
        {"exercise", st.exercise ? detail::exerciseToJsonValue(*st.exercise) : ojson(nullptr)},
        {"kit", kitJson(st.kit)},
        {"kits", std::move(kits)},
        {"mixer", std::move(mixer)},
        {"padMap", std::move(padMap)},
    });
}

std::string positionJson(bool playing, double posTicks, int64_t loop, double tickInLoop, double timeMs) {
    return detail::dump(ojson{{"type", "position"},
                              {"playing", playing},
                              {"posTicks", ticks(posTicks)},
                              {"loop", loop},
                              {"tickInLoop", ticks(tickInLoop)},
                              {"timeMs", ms(timeMs)}});
}

std::string hitJson(const ScoredHit& hit) {
    const bool free = hit.grade == Grade::Free;
    const bool matched = hit.grade == Grade::OnTime || hit.grade == Grade::Early || hit.grade == Grade::Late;
    return detail::dump(ojson{
        {"type", "hit"},
        {"instrument", toString(hit.instrument)},
        {"note", hit.note},
        {"velocity", hit.velocity},
        {"loop", free ? ojson(nullptr) : ojson(hit.loop)},
        {"tickInLoop", free ? ojson(nullptr) : ticks(hit.tickInLoop)},
        {"expectedTick", matched && hit.expectedTick ? ojson(*hit.expectedTick) : ojson(nullptr)},
        {"errorMs", matched && hit.errorMs ? ms(*hit.errorMs) : ojson(nullptr)},
        {"grade", toString(hit.grade)},
    });
}

std::string missedJson(const MissedNote& missed) {
    return detail::dump(ojson{{"type", "missed"},
                              {"instrument", toString(missed.instrument)},
                              {"loop", missed.loop},
                              {"expectedTick", missed.expectedTick}});
}

std::string loopSummaryJson(const LoopSummary& summary) {
    ojson instruments = ojson::object();
    for (const auto& [instrument, stats] : summary.perInstrument)
        instruments[std::string(toString(instrument))] = statsJson(stats);
    return detail::dump(ojson{{"type", "loopSummary"},
                              {"loop", summary.loop},
                              {"instruments", std::move(instruments)},
                              {"total", statsJson(summary.total)}});
}

std::string recordingSavedJson(const std::string& path) {
    return detail::dump(ojson{{"type", "recordingSaved"}, {"path", path}});
}

std::string errorJson(std::string_view message) {
    return detail::dump(ojson{{"type", "error"}, {"message", std::string(message)}});
}

}  // namespace drums::proto
