#include "drums/exercise.h"

#include "test_util.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <vector>

using namespace drums;
using nlohmann::json;

namespace {

std::vector<int64_t> ticksOf(const Exercise& ex, Instrument inst) {
    std::vector<int64_t> out;
    for (const auto& n : ex.notes)
        if (n.instrument == inst) out.push_back(n.tick);
    return out;
}

// parseExercise must throw, with a message containing every fragment.
void expectError(const std::string& text, std::initializer_list<std::string> fragments) {
    CAPTURE(text);
    try {
        parseExercise(text);
        FAIL("expected an error");
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        CAPTURE(message);
        for (const auto& f : fragments) CHECK(message.find(f) != std::string::npos);
    }
}

}  // namespace

TEST_CASE("exercise: groove-12-8.json") {
    const Exercise ex = loadExercise(test::repoPath("exercises/groove-12-8.json"));
    CHECK(ex.id == "groove-12-8-bar23");
    CHECK(ex.title == "12/8 groove (bar 23)");
    CHECK(ex.timeSignature.numerator == 12);
    CHECK(ex.timeSignature.denominator == 8);
    CHECK(ex.ppq == 480);
    CHECK(ex.barTicks() == 2880);
    CHECK(ex.lengthTicks() == 2880);
    CHECK(ex.beatTicks == 720);
    CHECK(ex.subdivisionTicks == 240);
    CHECK(ex.bars == 1);
    CHECK(ex.defaultBpm == 60);

    std::vector<int64_t> hihat;
    for (int k = 0; k < 12; ++k) hihat.push_back(k * 240);
    CHECK(ticksOf(ex, Instrument::HiHatClosed) == hihat);
    CHECK(ticksOf(ex, Instrument::Snare) == std::vector<int64_t>{960, 1440});
    CHECK(ticksOf(ex, Instrument::Kick) == std::vector<int64_t>{0, 1200, 2640});
    CHECK(ex.notes.size() == 17);
    for (const auto& n : ex.notes) CHECK(n.velocity == 100);

    // Sorted by tick, then instrument.
    for (size_t k = 1; k < ex.notes.size(); ++k) {
        const auto& a = ex.notes[k - 1];
        const auto& b = ex.notes[k];
        CHECK((a.tick < b.tick || (a.tick == b.tick && a.instrument < b.instrument)));
    }
    CHECK(S(ex.notes[0].instrument) == S(Instrument::Kick));
    CHECK(S(ex.notes[1].instrument) == S(Instrument::HiHatClosed));

    CHECK(ex.msPerTick(60) == doctest::Approx(60000.0 / (60 * 720)));
}

TEST_CASE("exercise: defaults") {
    const Exercise ex = parseExercise(R"({"timeSignature": [4, 4]})");
    CHECK(ex.id.empty());
    CHECK(ex.ppq == 480);
    CHECK(ex.beatTicks == 480);
    CHECK(ex.subdivisionTicks == 480);
    CHECK(ex.bars == 1);
    CHECK(ex.defaultBpm == 80);
    CHECK(ex.notes.empty());

    struct Case {
        int num, den, beat, subdivision;
    };
    for (Case c : {Case{4, 4, 480, 480}, Case{3, 4, 480, 480}, Case{2, 2, 960, 960}, Case{6, 8, 720, 240},
                   Case{9, 8, 720, 240}, Case{12, 8, 720, 240}, Case{7, 8, 240, 240}, Case{3, 8, 240, 240},
                   Case{5, 4, 480, 480}, Case{12, 16, 360, 120}}) {
        CAPTURE(c.num);
        CAPTURE(c.den);
        const Exercise e = parseExercise(R"({"timeSignature": [)" + std::to_string(c.num) + "," +
                                         std::to_string(c.den) + "]}");
        CHECK(e.beatTicks == c.beat);
        CHECK(e.subdivisionTicks == c.subdivision);
        CHECK(e.barTicks() == int64_t(c.num) * 1920 / c.den);
    }

    // Explicit values win; gridTicks defaults to subdivisionTicks.
    const Exercise e = parseExercise(R"({"timeSignature": [4, 4], "beatTicks": 960, "subdivisionTicks": 120,
                                        "bars": 2, "defaultBpm": 72.5, "grid": {"kick": "x.x"}})");
    CHECK(e.beatTicks == 960);
    CHECK(e.subdivisionTicks == 120);
    CHECK(e.bars == 2);
    CHECK(e.lengthTicks() == 3840);
    CHECK(e.defaultBpm == 72.5);
    CHECK(ticksOf(e, Instrument::Kick) == std::vector<int64_t>{0, 240});
}

TEST_CASE("exercise: grid characters, notes array, merge") {
    const Exercise ex = parseExercise(R"({
        // comments are allowed
        "id": "t", "timeSignature": [4, 4], "bars": 2, "gridTicks": 120,
        "grid": {
            "snare": "xXg.-| x-..",
            "kick":  "x"
        },
        "notes": [
            {"instrument": "crash", "tick": 0, "velocity": 110},
            {"instrument": "snare", "tick": 120, "velocity": 64},
            {"instrument": "rideBell", "tick": 3839}
        ]
    })");
    CHECK(ex.id == "t");
    CHECK(ex.title == "t");  // title defaults to id
    // tick 0: kick, snare, crash (instrument order); snare 120 overridden by the notes array.
    struct N {
        Instrument i;
        int64_t tick;
        int velocity;
    };
    const std::vector<N> expected = {
        {Instrument::Kick, 0, 100},       {Instrument::Snare, 0, 100},  {Instrument::Crash, 0, 110},
        {Instrument::Snare, 120, 64},     {Instrument::Snare, 240, 40}, {Instrument::Snare, 600, 100},
        {Instrument::RideBell, 3839, 100},
    };
    REQUIRE(ex.notes.size() == expected.size());
    for (size_t k = 0; k < expected.size(); ++k) {
        CAPTURE(k);
        CHECK(S(ex.notes[k].instrument) == S(expected[k].i));
        CHECK(ex.notes[k].tick == expected[k].tick);
        CHECK(ex.notes[k].velocity == expected[k].velocity);
    }

    // A grid row may be exactly as long as the exercise.
    const Exercise full = parseExercise(R"({"timeSignature": [12, 8], "grid": {"ride": "xxx|xxx|xxx|xxx"}})");
    CHECK(ticksOf(full, Instrument::Ride).size() == 12);
}

TEST_CASE("exercise: validation errors name the field") {
    expectError("{", {"invalid JSON"});
    expectError("[1, 2]", {"JSON object"});
    expectError(R"({})", {"timeSignature", "missing"});
    expectError(R"({"timeSignature": [12]})", {"timeSignature"});
    expectError(R"({"timeSignature": "12/8"})", {"timeSignature"});
    expectError(R"({"timeSignature": [12, 7]})", {"timeSignature", "denominator"});
    expectError(R"({"timeSignature": [0, 4]})", {"timeSignature", "numerator"});
    expectError(R"({"timeSignature": [4, 4], "ppq": 960})", {"ppq", "480"});
    expectError(R"({"timeSignature": [4, 4], "bars": 0})", {"bars"});
    expectError(R"({"timeSignature": [4, 4], "bars": "two"})", {"bars"});
    expectError(R"({"timeSignature": [4, 4], "beatTicks": -1})", {"beatTicks"});
    expectError(R"({"timeSignature": [4, 4], "subdivisionTicks": 0})", {"subdivisionTicks"});
    expectError(R"({"timeSignature": [4, 4], "defaultBpm": 1000})", {"defaultBpm"});
    expectError(R"({"timeSignature": [4, 4], "id": 5})", {"id"});

    // grid
    expectError(R"({"timeSignature": [4, 4], "grid": {"cowbell": "x..."}})", {"grid.cowbell", "unknown instrument"});
    expectError(R"({"timeSignature": [4, 4], "grid": {"kick": "x.y."}})", {"grid.kick", "'y'", "position 3"});
    expectError(R"({"timeSignature": [4, 4], "grid": {"kick": 5}})", {"grid.kick", "string"});
    expectError(R"({"timeSignature": [4, 4], "grid": ["x"]})", {"grid"});
    expectError(R"({"timeSignature": [12, 8], "gridTicks": 240, "grid": {"snare": "xxx|xxx|xxx|xxx|x"}})",
                {"grid.snare", "too long", "12 steps"});
    expectError(R"({"timeSignature": [4, 4], "gridTicks": 0, "grid": {"kick": "x"}})", {"gridTicks"});

    // notes
    expectError(R"({"timeSignature": [4, 4], "notes": {}})", {"notes", "array"});
    expectError(R"({"timeSignature": [4, 4], "notes": [5]})", {"notes[0]"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 0}, {"instrument": "gong", "tick": 0}]})",
                {"notes[1].instrument", "unknown instrument \"gong\""});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"tick": 0}]})", {"notes[0].instrument"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick"}]})", {"notes[0].tick", "missing"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": -1}]})", {"notes[0].tick"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 1.5}]})", {"notes[0].tick"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 1920}]})",
                {"notes[0].tick", "beyond", "bars"});
    expectError(R"({"timeSignature": [4, 4], "bars": 2, "notes": [{"instrument": "kick", "tick": 3840}]})",
                {"notes[0].tick", "beyond"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 0, "velocity": 0}]})",
                {"notes[0].velocity", "1 and 127"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 0, "velocity": 128}]})",
                {"notes[0].velocity"});
    expectError(R"({"timeSignature": [4, 4], "notes": [{"instrument": "kick", "tick": 0, "velocity": "loud"}]})",
                {"notes[0].velocity"});
}

TEST_CASE("exercise: loadExercise errors and id from the file name") {
    auto dir = test::tempDir("exercise-load");
    CHECK_THROWS_WITH_AS(loadExercise((dir / "missing.json").string()), doctest::Contains("missing.json"),
                         std::runtime_error);

    test::writeFile(dir / "bad.json", R"({"timeSignature": [4, 4], "notes": [{"instrument": "x", "tick": 0}]})");
    CHECK_THROWS_WITH_AS(loadExercise((dir / "bad.json").string()), doctest::Contains("bad.json: notes[0].instrument"),
                         std::runtime_error);

    test::writeFile(dir / "my-groove.json", R"({"timeSignature": [4, 4], "title": "Mine"})");
    const Exercise ex = loadExercise((dir / "my-groove.json").string());
    CHECK(ex.id == "my-groove");
    CHECK(ex.title == "Mine");
}

TEST_CASE("exercise: exerciseToJson is canonical and round-trips") {
    const Exercise ex = loadExercise(test::repoPath("exercises/groove-12-8.json"));
    const json j = json::parse(exerciseToJson(ex));
    CHECK(j["id"] == "groove-12-8-bar23");
    CHECK(j["title"] == "12/8 groove (bar 23)");
    CHECK(j["timeSignature"] == json::array({12, 8}));
    CHECK(j["ppq"] == 480);
    CHECK(j["beatTicks"] == 720);
    CHECK(j["subdivisionTicks"] == 240);
    CHECK(j["bars"] == 1);
    CHECK(j["defaultBpm"] == 60);
    CHECK(j["defaultBpm"].is_number_integer());
    CHECK_FALSE(j.contains("grid"));
    CHECK_FALSE(j.contains("gridTicks"));
    REQUIRE(j["notes"].size() == 17);
    CHECK(j["notes"][0] == json{{"instrument", "kick"}, {"tick", 0}, {"velocity", 100}});
    CHECK(j["notes"][1] == json{{"instrument", "hihatClosed"}, {"tick", 0}, {"velocity", 100}});

    const Exercise again = parseExercise(exerciseToJson(ex));
    CHECK(again.id == ex.id);
    CHECK(again.beatTicks == ex.beatTicks);
    CHECK(again.defaultBpm == ex.defaultBpm);
    REQUIRE(again.notes.size() == ex.notes.size());
    for (size_t k = 0; k < ex.notes.size(); ++k) {
        CHECK(S(again.notes[k].instrument) == S(ex.notes[k].instrument));
        CHECK(again.notes[k].tick == ex.notes[k].tick);
        CHECK(again.notes[k].velocity == ex.notes[k].velocity);
    }
}

TEST_CASE("pad map: load kits/pad-map.json") {
    const PadMap map = loadPadMap(test::repoPath("kits/pad-map.json"));
    CHECK(map.hihatPedalCC == 4);
    CHECK(S(map[35]) == S(Instrument::Kick));
    CHECK(S(map[36]) == S(Instrument::Kick));
    CHECK(S(map[38]) == S(Instrument::Snare));
    CHECK(S(map[37]) == S(Instrument::SnareRim));
    CHECK(S(map[42]) == S(Instrument::HiHatClosed));
    CHECK(S(map[46]) == S(Instrument::HiHatOpen));
    CHECK(S(map[44]) == S(Instrument::HiHatPedal));
    CHECK(S(map[48]) == S(Instrument::Tom1));
    CHECK(S(map[45]) == S(Instrument::Tom2));
    CHECK(S(map[41]) == S(Instrument::Tom3));
    CHECK(S(map[49]) == S(Instrument::Crash));
    CHECK(S(map[51]) == S(Instrument::Ride));
    CHECK(S(map[53]) == S(Instrument::RideBell));
    CHECK(S(map[60]) == S(Instrument::Unknown));
    CHECK(S(map[0]) == S(Instrument::Unknown));
    int mapped = 0;
    for (auto i : map.byNote) mapped += i != Instrument::Unknown;
    CHECK(mapped == 24);
}

TEST_CASE("pad map: save and reload, keeping the comment") {
    auto dir = test::tempDir("padmap");
    const auto path = dir / "nested" / "pad-map.json";
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::copy_file(test::repoPath("kits/pad-map.json"), path);

    PadMap map = loadPadMap(path.string());
    map.byNote[60] = Instrument::Tom1;
    map.byNote[36] = Instrument::Unknown;
    map.hihatPedalCC = 11;
    REQUIRE(savePadMap(map, path.string()));

    const json j = json::parse(test::readFile(path));
    CHECK(j["comment"].get<std::string>().find("e-drum MIDI note") == 0);
    CHECK(j["hihatPedalCC"] == 11);
    CHECK(j["notes"]["60"] == "tom1");
    CHECK_FALSE(j["notes"].contains("36"));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    const PadMap again = loadPadMap(path.string());
    CHECK(again.byNote == map.byNote);
    CHECK(again.hihatPedalCC == 11);

    // A new file in a directory that does not exist yet; notes in numeric order.
    PadMap small;
    small.byNote[100] = Instrument::Crash;
    small.byNote[9] = Instrument::Kick;
    const auto fresh = dir / "a" / "b" / "map.json";
    REQUIRE(savePadMap(small, fresh.string()));
    const std::string text = test::readFile(fresh);
    CHECK(text.find("\"9\"") < text.find("\"100\""));
    CHECK(loadPadMap(fresh.string()).byNote == small.byNote);

    CHECK_FALSE(savePadMap(small, "/proc/drums-no-such-dir/map.json"));
}

TEST_CASE("pad map: errors") {
    auto dir = test::tempDir("padmap-errors");
    auto check = [&](const std::string& text, const std::string& fragment) {
        CAPTURE(text);
        test::writeFile(dir / "m.json", text);
        CHECK_THROWS_WITH_AS(loadPadMap((dir / "m.json").string()), doctest::Contains(fragment.c_str()), std::runtime_error);
    };
    check(R"({"notes": {"abc": "kick"}})", "notes.\"abc\"");
    check(R"({"notes": {"128": "kick"}})", "notes.\"128\"");
    check(R"({"notes": {"36": "cowbell"}})", "unknown instrument \"cowbell\"");
    check(R"({"notes": {"36": 5}})", "notes.\"36\"");
    check(R"({"hihatPedalCC": 200, "notes": {}})", "hihatPedalCC");
    check(R"({})", "notes");
    check(R"(nope)", "invalid JSON");
    CHECK_THROWS_AS(loadPadMap((dir / "absent.json").string()), std::runtime_error);

    test::writeFile(dir / "null.json", R"({"notes": {"36": null, "38": "snare"}})");
    const PadMap map = loadPadMap((dir / "null.json").string());
    CHECK(S(map[36]) == S(Instrument::Unknown));
    CHECK(S(map[38]) == S(Instrument::Snare));
}
