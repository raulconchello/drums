#include "drums/protocol.h"

#include "test_util.h"

#include <nlohmann/json.hpp>

using namespace drums;
using namespace drums::proto;
using nlohmann::json;

namespace {

template <class T>
T parseAs(const std::string& text) {
    CAPTURE(text);
    std::string error;
    auto cmd = parseCommand(text, &error);
    CAPTURE(error);
    REQUIRE(cmd.has_value());
    REQUIRE(std::holds_alternative<T>(*cmd));
    CHECK(error.empty());
    return std::get<T>(*cmd);
}

void rejects(const std::string& text, const std::string& fragment) {
    CAPTURE(text);
    std::string error;
    auto cmd = parseCommand(text, &error);
    CAPTURE(error);
    CHECK_FALSE(cmd.has_value());
    CHECK(error.find(fragment) != std::string::npos);
}

}  // namespace

TEST_CASE("protocol: parse every command type") {
    parseAs<Play>(R"({"type": "play"})");
    parseAs<Stop>(R"({"type": "stop"})");
    parseAs<StartRecording>(R"({"type": "startRecording"})");
    parseAs<StopRecording>(R"({"type": "stopRecording"})");
    parseAs<GetState>(R"({"type": "getState", "extra": "ignored"})");

    CHECK(parseAs<SetBpm>(R"({"type": "setBpm", "bpm": 72.5})").bpm == 72.5);
    CHECK(parseAs<SetBpm>(R"({"type": "setBpm", "bpm": 20})").bpm == 20);
    CHECK(parseAs<SetBpm>(R"({"type": "setBpm", "bpm": 300})").bpm == 300);
    CHECK(parseAs<SetLoop>(R"({"type": "setLoop", "on": false})").on == false);
    CHECK(parseAs<SetLoop>(R"({"type": "setLoop", "on": true})").on == true);
    CHECK(parseAs<SetCountIn>(R"({"type": "setCountIn", "bars": 0})").bars == 0);
    CHECK(parseAs<SetCountIn>(R"({"type": "setCountIn", "bars": 2.0})").bars == 2);

    SetClick click = parseAs<SetClick>(R"({"type": "setClick", "volume": 0.5})");
    CHECK_FALSE(click.on);
    CHECK(click.volume == 0.5);
    CHECK_FALSE(click.subdivisions);
    click = parseAs<SetClick>(R"({"type": "setClick", "on": false, "volume": 1, "subdivisions": true})");
    CHECK(click.on == false);
    CHECK(click.volume == 1.0);
    CHECK(click.subdivisions == true);

    SetGroove groove = parseAs<SetGroove>(R"({"type": "setGroove", "on": true})");
    CHECK(groove.on == true);
    CHECK_FALSE(groove.volume);
    groove = parseAs<SetGroove>(R"({"type": "setGroove", "volume": 0})");
    CHECK(groove.volume == 0.0);

    CHECK(parseAs<LoadExercise>(R"({"type": "loadExercise", "path": "exercises/groove-12-8.json"})").path ==
          "exercises/groove-12-8.json");
    CHECK(parseAs<LoadKit>(R"({"type": "loadKit", "path": "/abs/kit/kit.json"})").path == "/abs/kit/kit.json");

    SetMixer mixer = parseAs<SetMixer>(R"({"type": "setMixer", "channel": "master", "volume": 1.5})");
    CHECK(mixer.channel == "master");
    CHECK(mixer.volume == 1.5);
    CHECK(parseAs<SetMixer>(R"({"type": "setMixer", "channel": "hihatClosed", "volume": 0})").channel == "hihatClosed");

    CHECK(parseAs<SetLatencyOffset>(R"({"type": "setLatencyOffset", "ms": -50})").ms == -50);
    CHECK(parseAs<SetLatencyOffset>(R"({"type": "setLatencyOffset", "ms": 12.5})").ms == 12.5);

    SetPadMap pad = parseAs<SetPadMap>(R"({"type": "setPadMap", "note": 36, "instrument": "kick"})");
    CHECK(pad.note == 36);
    CHECK(S(pad.instrument) == S(Instrument::Kick));
    pad = parseAs<SetPadMap>(R"({"type": "setPadMap", "note": 127, "instrument": null})");
    CHECK(pad.note == 127);
    CHECK(S(pad.instrument) == S(Instrument::Unknown));

    // The error pointer is optional.
    CHECK(parseCommand(R"({"type": "play"})").has_value());
    CHECK_FALSE(parseCommand("nope").has_value());
}

TEST_CASE("protocol: malformed and out-of-range commands") {
    rejects("", "invalid JSON");
    rejects("{", "invalid JSON");
    rejects("[]", "JSON object");
    rejects("{}", "\"type\"");
    rejects(R"({"type": 5})", "\"type\"");
    rejects(R"({"type": "dance"})", "unknown command type \"dance\"");

    rejects(R"({"type": "setBpm"})", "setBpm: missing \"bpm\"");
    rejects(R"({"type": "setBpm", "bpm": "fast"})", "setBpm: \"bpm\" must be a number between 20 and 300");
    rejects(R"({"type": "setBpm", "bpm": 19.9})", "between 20 and 300");
    rejects(R"({"type": "setBpm", "bpm": 301})", "between 20 and 300");
    rejects(R"({"type": "setLoop"})", "setLoop: missing \"on\"");
    rejects(R"({"type": "setLoop", "on": 1})", "true or false");
    rejects(R"({"type": "setCountIn", "bars": 3})", "between 0 and 2");
    rejects(R"({"type": "setCountIn", "bars": -1})", "between 0 and 2");
    rejects(R"({"type": "setCountIn", "bars": 1.5})", "integer");
    rejects(R"({"type": "setClick"})", "at least one");
    rejects(R"({"type": "setClick", "volume": 1.01})", "setClick: \"volume\" must be a number between 0 and 1");
    rejects(R"({"type": "setClick", "on": "yes"})", "setClick: \"on\"");
    rejects(R"({"type": "setClick", "subdivisions": null})", "subdivisions");
    rejects(R"({"type": "setGroove", "volume": -0.1})", "setGroove: \"volume\"");
    rejects(R"({"type": "setGroove"})", "at least one");
    rejects(R"({"type": "loadExercise"})", "loadExercise: missing \"path\"");
    rejects(R"({"type": "loadExercise", "path": ""})", "non-empty");
    rejects(R"({"type": "loadKit", "path": 3})", "loadKit");
    rejects(R"({"type": "setMixer", "volume": 1})", "missing \"channel\"");
    rejects(R"({"type": "setMixer", "channel": "cowbell", "volume": 1})", "unknown channel \"cowbell\"");
    rejects(R"({"type": "setMixer", "channel": "kick", "volume": 1.6})", "between 0 and 1.5");
    rejects(R"({"type": "setMixer", "channel": "kick"})", "missing \"volume\"");
    rejects(R"({"type": "setLatencyOffset", "ms": -51})", "between -50 and 100");
    rejects(R"({"type": "setLatencyOffset", "ms": 100.5})", "between -50 and 100");
    rejects(R"({"type": "setPadMap", "note": 128, "instrument": "kick"})", "between 0 and 127");
    rejects(R"({"type": "setPadMap", "note": -1, "instrument": "kick"})", "\"note\"");
    rejects(R"({"type": "setPadMap", "note": "36", "instrument": "kick"})", "\"note\"");
    rejects(R"({"type": "setPadMap", "note": 36})", "missing \"instrument\"");
    rejects(R"({"type": "setPadMap", "note": 36, "instrument": "gong"})", "unknown instrument \"gong\"");
    rejects(R"({"type": "setPadMap", "note": 36, "instrument": 4})", "\"instrument\"");
}

TEST_CASE("protocol: hello, position, recordingSaved, error") {
    json j = json::parse(helloJson(48000, 64, 2.6666));
    CHECK(j == json{{"type", "hello"}, {"protocol", 1}, {"sampleRate", 48000}, {"bufferFrames", 64}, {"outputLatencyMs", 2.7}});

    const std::string pos = positionJson(true, -1439.123456, -1, 1440.876544, 12345.6789);
    j = json::parse(pos);
    CHECK(j["type"] == "position");
    CHECK(j["playing"] == true);
    CHECK(j["posTicks"] == -1439.12);
    CHECK(j["loop"] == -1);
    CHECK(j["tickInLoop"] == 1440.88);
    CHECK(j["timeMs"] == 12345.7);
    CHECK(pos.find("1439.12,") != std::string::npos);  // short numbers on the wire
    CHECK(pos.rfind(R"({"type":"position")", 0) == 0);  // "type" first

    j = json::parse(positionJson(false, 2880, 1, 0, 4000));
    CHECK(j["posTicks"].is_number_integer());
    CHECK(j["playing"] == false);

    j = json::parse(recordingSavedJson("recordings/2026-11-15 take.mid"));
    CHECK(j == json{{"type", "recordingSaved"}, {"path", "recordings/2026-11-15 take.mid"}});

    j = json::parse(errorJson("cannot load \"x\"\n"));
    CHECK(j == json{{"type", "error"}, {"message", "cannot load \"x\"\n"}});
    // Invalid UTF-8 is replaced instead of throwing.
    j = json::parse(errorJson(std::string("bad \xff path")));
    CHECK(j["type"] == "error");
}

TEST_CASE("protocol: hit") {
    ScoredHit h;
    h.instrument = Instrument::Snare;
    h.note = 38;
    h.velocity = 97;
    h.loop = 3;
    h.tickInLoop = 958.3333;
    h.expectedTick = 960;
    h.errorMs = -2.3149;
    h.grade = Grade::OnTime;
    json j = json::parse(hitJson(h));
    CHECK(j == json{{"type", "hit"}, {"instrument", "snare"}, {"note", 38}, {"velocity", 97}, {"loop", 3},
                    {"tickInLoop", 958.33}, {"expectedTick", 960}, {"errorMs", -2.3}, {"grade", "onTime"}});

    h.grade = Grade::Early;
    CHECK(json::parse(hitJson(h))["grade"] == "early");
    h.grade = Grade::Late;
    CHECK(json::parse(hitJson(h))["grade"] == "late");

    // Extra: expectedTick and errorMs null (even if set).
    h.grade = Grade::Extra;
    j = json::parse(hitJson(h));
    CHECK(j["grade"] == "extra");
    CHECK(j["expectedTick"].is_null());
    CHECK(j["errorMs"].is_null());
    CHECK(j["loop"] == 3);
    CHECK(j["tickInLoop"] == 958.33);

    // Free: loop and tickInLoop null too.
    h.grade = Grade::Free;
    j = json::parse(hitJson(h));
    CHECK(j["grade"] == "free");
    CHECK(j["loop"].is_null());
    CHECK(j["tickInLoop"].is_null());
    CHECK(j["expectedTick"].is_null());
    CHECK(j["errorMs"].is_null());
    CHECK(j.size() == 9);  // every field present

    // Count-in hit: negative loop.
    ScoredHit early;
    early.instrument = Instrument::HiHatClosed;
    early.loop = -1;
    early.tickInLoop = 2858.4;
    early.expectedTick = 0;
    early.errorMs = -30;
    early.grade = Grade::Early;
    j = json::parse(hitJson(early));
    CHECK(j["loop"] == -1);
    CHECK(j["expectedTick"] == 0);
    CHECK(j["errorMs"] == -30);
    CHECK(j["instrument"] == "hihatClosed");
}

TEST_CASE("protocol: missed and loopSummary") {
    json j = json::parse(missedJson({Instrument::Kick, 2, 1200}));
    CHECK(j == json{{"type", "missed"}, {"instrument", "kick"}, {"loop", 2}, {"expectedTick", 1200}});

    LoopSummary s;
    s.loop = 4;
    s.perInstrument[Instrument::Snare] = {2, 1, 1, -3.04, 1.96};
    s.perInstrument[Instrument::HiHatOpen] = {1, 0, 0, 10, 0};
    s.total = {3, 1, 1, 1.333333, 6.0123};
    j = json::parse(loopSummaryJson(s));
    CHECK(j["type"] == "loopSummary");
    CHECK(j["loop"] == 4);
    CHECK(j["instruments"].size() == 2);
    CHECK(j["instruments"]["snare"] ==
          json{{"hits", 2}, {"missed", 1}, {"extra", 1}, {"meanErrorMs", -3.0}, {"stdDevMs", 2.0}});
    CHECK(j["instruments"]["hihatOpen"]["meanErrorMs"] == 10);
    CHECK(j["total"] == json{{"hits", 3}, {"missed", 1}, {"extra", 1}, {"meanErrorMs", 1.3}, {"stdDevMs", 6.0}});

    s.perInstrument.clear();
    j = json::parse(loopSummaryJson(s));
    CHECK(j["instruments"].is_object());
    CHECK(j["instruments"].empty());
}

TEST_CASE("protocol: state") {
    EngineState st;
    json j = json::parse(stateJson(st));
    CHECK(j["type"] == "state");
    CHECK(j["playing"] == false);
    CHECK(j["recording"] == false);
    CHECK(j["bpm"] == 60);
    CHECK(j["loop"] == true);
    CHECK(j["countInBars"] == 1);
    CHECK(j["click"] == json{{"on", true}, {"volume", 0.8}, {"subdivisions", true}});
    CHECK(j["groove"] == json{{"on", false}, {"volume", 0.7}});
    CHECK(j["latencyOffsetMs"] == 0);
    CHECK(j["exercise"].is_null());
    CHECK(j["kit"] == json{{"name", ""}, {"path", ""}});
    CHECK(j["kits"] == json::array());
    CHECK(j["mixer"] == json::object());
    CHECK(j["padMap"] == json::object());

    st.playing = true;
    st.recording = true;
    st.bpm = 72.5;
    st.loop = false;
    st.countInBars = 2;
    st.clickOn = false;
    st.clickVolume = 0.25;
    st.clickSubdivisions = false;
    st.grooveOn = true;
    st.grooveVolume = 1;
    st.latencyOffsetMs = -12.5;
    st.exercise = loadExercise(test::repoPath("exercises/groove-12-8.json"));
    st.kit = {"Kit A", "kits/a/kit.json"};
    st.kits = {{"Kit A", "kits/a/kit.json"}, {"Kit B", "kits/b/kit.json"}};
    st.mixer = {{"master", 0.8}, {"kick", 1.0}, {"snare", 1.25}};
    st.padMap.byNote[36] = Instrument::Kick;
    st.padMap.byNote[38] = Instrument::Snare;
    j = json::parse(stateJson(st));
    CHECK(j["playing"] == true);
    CHECK(j["recording"] == true);
    CHECK(j["bpm"] == 72.5);
    CHECK(j["loop"] == false);
    CHECK(j["countInBars"] == 2);
    CHECK(j["click"] == json{{"on", false}, {"volume", 0.25}, {"subdivisions", false}});
    CHECK(j["groove"] == json{{"on", true}, {"volume", 1}});
    CHECK(j["latencyOffsetMs"] == -12.5);
    CHECK(j["kit"] == json{{"name", "Kit A"}, {"path", "kits/a/kit.json"}});
    CHECK(j["kits"].size() == 2);
    CHECK(j["kits"][1]["name"] == "Kit B");
    CHECK(j["mixer"] == json{{"master", 0.8}, {"kick", 1}, {"snare", 1.25}});
    CHECK(j["padMap"] == json{{"36", "kick"}, {"38", "snare"}});

    const json& ex = j["exercise"];
    CHECK(ex["id"] == "groove-12-8-bar23");
    CHECK(ex["timeSignature"] == json::array({12, 8}));
    CHECK(ex["ppq"] == 480);
    CHECK(ex["beatTicks"] == 720);
    CHECK(ex["subdivisionTicks"] == 240);
    CHECK(ex["bars"] == 1);
    CHECK(ex["defaultBpm"] == 60);
    CHECK(ex["notes"].size() == 17);
    CHECK_FALSE(ex.contains("grid"));
    CHECK(ex == json::parse(exerciseToJson(*st.exercise)));
}
