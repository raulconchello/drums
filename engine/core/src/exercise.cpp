#include "drums/exercise.h"

#include "json_util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace drums {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr int kSupportedPpq = 480;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

// Short rendering of a JSON value for error messages.
std::string show(const json& v) {
    std::string s = detail::dump(v);
    if (s.size() > 40) s = s.substr(0, 37) + "...";
    return s;
}

std::string instrumentList() {
    std::string s;
    for (Instrument i : kInstruments) {
        if (!s.empty()) s += ' ';
        s += toString(i);
    }
    return s;
}

std::optional<int64_t> asInteger(const json& v) {
    if (v.is_number_unsigned()) {
        auto u = v.get<uint64_t>();
        if (u > uint64_t(std::numeric_limits<int64_t>::max())) return std::nullopt;
        return int64_t(u);
    }
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e15) return int64_t(d);
    }
    return std::nullopt;
}

// Optional integer field of `obj`, checked against [min, max].
int64_t intField(const json& obj, const std::string& path, const char* key, int64_t def,
                 int64_t min, int64_t max) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    auto v = asInteger(*it);
    if (!v || *v < min || *v > max) {
        std::ostringstream ss;
        ss << path << key << ": must be an integer between " << min << " and " << max
           << " (got " << show(*it) << ")";
        fail(ss.str());
    }
    return *v;
}

double numberField(const json& obj, const char* key, double def, double min, double max) {
    auto it = obj.find(key);
    if (it == obj.end()) return def;
    if (!it->is_number() || it->get<double>() < min || it->get<double>() > max) {
        std::ostringstream ss;
        ss << key << ": must be a number between " << min << " and " << max << " (got "
           << show(*it) << ")";
        fail(ss.str());
    }
    return it->get<double>();
}

std::string stringField(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (!it->is_string()) fail(std::string(key) + ": must be a string (got " + show(*it) + ")");
    return it->get<std::string>();
}

Instrument instrumentNamed(const std::string& name, const std::string& where) {
    Instrument i = instrumentFromString(name);
    if (i == Instrument::Unknown)
        fail(where + ": unknown instrument \"" + name + "\" (known: " + instrumentList() + ")");
    return i;
}

json parseJsonText(std::string_view text) {
    try {
        return json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true,
                           /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        std::string what = e.what();
        // Drop nlohmann's "[json.exception.parse_error.101] " prefix.
        if (auto p = what.find("] "); what.rfind("[json.exception", 0) == 0 && p != std::string::npos)
            what = what.substr(p + 2);
        fail("invalid JSON: " + what);
    }
}

std::string readFile(const std::string& path, const char* what) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail(std::string("cannot open ") + what + " \"" + path + "\"");
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) fail(std::string("cannot read ") + what + " \"" + path + "\"");
    return ss.str();
}

bool isCompoundMeter(const TimeSignature& ts) {
    return ts.denominator >= 8 && ts.numerator > 3 && ts.numerator % 3 == 0;
}

struct ParsedNote {
    ExerciseNote note;
    int source = 0;  // 0 = grid, 1 = notes array; later sources win on duplicates
    size_t order = 0;
};

}  // namespace

Exercise parseExercise(std::string_view jsonText) {
    json root = parseJsonText(jsonText);
    if (!root.is_object()) fail("an exercise must be a JSON object (got " + show(root) + ")");

    Exercise ex;
    ex.id = stringField(root, "id");
    ex.title = stringField(root, "title");
    if (ex.title.empty()) ex.title = ex.id;

    // timeSignature: [numerator, denominator]
    {
        auto it = root.find("timeSignature");
        if (it == root.end()) fail("timeSignature: missing (e.g. [12, 8])");
        const json& ts = *it;
        std::optional<int64_t> num, den;
        if (ts.is_array() && ts.size() == 2) {
            num = asInteger(ts[0]);
            den = asInteger(ts[1]);
        }
        if (!num || !den)
            fail("timeSignature: must be [numerator, denominator] (got " + show(ts) + ")");
        if (*num < 1 || *num > 64)
            fail("timeSignature: numerator must be between 1 and 64 (got " + std::to_string(*num) + ")");
        if (*den < 1 || *den > 64 || (*den & (*den - 1)) != 0)
            fail("timeSignature: denominator must be 1, 2, 4, 8, 16, 32 or 64 (got " +
                 std::to_string(*den) + ")");
        ex.timeSignature = {int(*num), int(*den)};
    }

    ex.ppq = int(intField(root, "", "ppq", kSupportedPpq, 1, 1 << 20));
    if (ex.ppq != kSupportedPpq)
        fail("ppq: only " + std::to_string(kSupportedPpq) + " is supported (got " +
             std::to_string(ex.ppq) + ")");

    const int denominatorTicks = ex.ppq * 4 / ex.timeSignature.denominator;
    const int defaultBeat = isCompoundMeter(ex.timeSignature) ? 3 * denominatorTicks : denominatorTicks;
    ex.beatTicks = int(intField(root, "", "beatTicks", defaultBeat, 1, ex.ppq * 16));
    ex.subdivisionTicks = int(intField(root, "", "subdivisionTicks", denominatorTicks, 1, ex.ppq * 16));
    ex.bars = int(intField(root, "", "bars", 1, 1, 1000));
    ex.defaultBpm = numberField(root, "defaultBpm", 80.0, 20.0, 300.0);

    const int64_t length = ex.lengthTicks();
    std::vector<ParsedNote> parsed;

    // grid: one string per instrument, one character per gridTicks step
    if (auto git = root.find("grid"); git != root.end() && !git->is_null()) {
        if (!git->is_object()) fail("grid: must be an object {instrument: \"x..x\"} (got " + show(*git) + ")");
        const int64_t gridTicks = intField(root, "", "gridTicks", ex.subdivisionTicks, 1, length);
        const int64_t maxSteps = (length + gridTicks - 1) / gridTicks;
        for (auto& [name, row] : git->items()) {
            const std::string where = "grid." + name;
            Instrument inst = instrumentNamed(name, where);
            if (!row.is_string()) fail(where + ": must be a string like \"x..|x..\" (got " + show(row) + ")");
            const std::string s = row.get<std::string>();
            int64_t step = 0;
            for (size_t pos = 0; pos < s.size(); ++pos) {
                const char c = s[pos];
                int velocity = 0;
                switch (c) {
                    case '|': case ' ': case '\t': continue;
                    case '.': case '-': velocity = 0; break;
                    case 'x': velocity = 100; break;
                    case 'X': velocity = 124; break;
                    case 'g': velocity = 40; break;
                    default: {
                        const auto byte = static_cast<unsigned char>(c);
                        std::string shown(1, c);
                        if (byte < 0x20 || byte >= 0x7f) {
                            static const char* hex = "0123456789abcdef";
                            shown = std::string("\\x") + hex[byte >> 4] + hex[byte & 15];
                        }
                        fail(where + ": unexpected character '" + shown + "' at position " +
                             std::to_string(pos + 1) + " (use x X g . - and | or spaces as separators)");
                    }
                }
                if (step >= maxSteps)
                    fail(where + ": too long, more than " + std::to_string(maxSteps) + " steps (bars " +
                         std::to_string(ex.bars) + " x " + std::to_string(ex.barTicks()) +
                         " ticks / gridTicks " + std::to_string(gridTicks) + ")");
                if (velocity > 0)
                    parsed.push_back({{inst, step * gridTicks, velocity}, 0, parsed.size()});
                ++step;
            }
        }
    } else if (root.contains("gridTicks")) {
        intField(root, "", "gridTicks", 1, 1, length);  // still validate it
    }

    // notes: explicit notes
    if (auto nit = root.find("notes"); nit != root.end() && !nit->is_null()) {
        if (!nit->is_array()) fail("notes: must be an array (got " + show(*nit) + ")");
        for (size_t k = 0; k < nit->size(); ++k) {
            const json& n = (*nit)[k];
            const std::string where = "notes[" + std::to_string(k) + "]";
            if (!n.is_object())
                fail(where + ": must be an object {\"instrument\", \"tick\", \"velocity\"} (got " + show(n) + ")");
            auto iit = n.find("instrument");
            if (iit == n.end() || !iit->is_string())
                fail(where + ".instrument: missing or not a string");
            Instrument inst = instrumentNamed(iit->get<std::string>(), where + ".instrument");

            auto tit = n.find("tick");
            if (tit == n.end()) fail(where + ".tick: missing");
            auto tick = asInteger(*tit);
            if (!tick || *tick < 0) fail(where + ".tick: must be an integer >= 0 (got " + show(*tit) + ")");
            if (*tick >= length)
                fail(where + ".tick: " + std::to_string(*tick) + " is beyond the end of the exercise (bars " +
                     std::to_string(ex.bars) + " = " + std::to_string(length) + " ticks); increase \"bars\"");

            const int velocity = int(intField(n, where + ".", "velocity", 100, 1, 127));
            parsed.push_back({{inst, *tick, velocity}, 1, parsed.size()});
        }
    }

    // Merge: sort by tick then instrument. A note given twice (same tick and instrument) is kept
    // once; the notes array overrides the grid, a later entry overrides an earlier one.
    std::sort(parsed.begin(), parsed.end(), [](const ParsedNote& a, const ParsedNote& b) {
        if (a.note.tick != b.note.tick) return a.note.tick < b.note.tick;
        if (a.note.instrument != b.note.instrument) return a.note.instrument < b.note.instrument;
        if (a.source != b.source) return a.source < b.source;
        return a.order < b.order;
    });
    for (size_t k = 0; k < parsed.size(); ++k) {
        const bool overridden = k + 1 < parsed.size() && parsed[k + 1].note.tick == parsed[k].note.tick &&
                                parsed[k + 1].note.instrument == parsed[k].note.instrument;
        if (!overridden) ex.notes.push_back(parsed[k].note);
    }
    return ex;
}

Exercise loadExercise(const std::string& path) {
    std::string text = readFile(path, "exercise file");
    Exercise ex;
    try {
        ex = parseExercise(text);
    } catch (const std::exception& e) {
        fail(path + ": " + e.what());
    }
    if (ex.id.empty()) ex.id = fs::path(path).stem().string();
    if (ex.title.empty()) ex.title = ex.id;
    return ex;
}

namespace detail {

ojson exerciseToJsonValue(const Exercise& ex) {
    ojson notes = ojson::array();
    for (const ExerciseNote& n : ex.notes)
        notes.push_back({{"instrument", toString(n.instrument)}, {"tick", n.tick}, {"velocity", n.velocity}});
    return ojson{
        {"id", ex.id},
        {"title", ex.title},
        {"timeSignature", {ex.timeSignature.numerator, ex.timeSignature.denominator}},
        {"ppq", ex.ppq},
        {"beatTicks", ex.beatTicks},
        {"subdivisionTicks", ex.subdivisionTicks},
        {"bars", ex.bars},
        {"defaultBpm", plainNumber(ex.defaultBpm)},
        {"notes", std::move(notes)},
    };
}

}  // namespace detail

std::string exerciseToJson(const Exercise& exercise) {
    return detail::dump(detail::exerciseToJsonValue(exercise));
}

// ---- pad map ----

PadMap loadPadMap(const std::string& path) {
    std::string text = readFile(path, "pad map");
    try {
        json root = parseJsonText(text);
        if (!root.is_object()) fail("a pad map must be a JSON object");
        PadMap map;
        map.hihatPedalCC = int(intField(root, "", "hihatPedalCC", map.hihatPedalCC, 0, 127));
        auto nit = root.find("notes");
        if (nit == root.end()) fail("notes: missing (e.g. {\"36\": \"kick\"})");
        if (!nit->is_object()) fail("notes: must be an object {\"36\": \"kick\"} (got " + show(*nit) + ")");
        for (auto& [key, value] : nit->items()) {
            const std::string where = "notes.\"" + key + "\"";
            int note = -1;
            if (!key.empty() && key.size() <= 3 &&
                std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }))
                note = std::stoi(key);
            if (note < 0 || note > 127) fail(where + ": the key must be a MIDI note number 0-127");
            if (value.is_null()) continue;  // explicitly unmapped
            if (!value.is_string()) fail(where + ": must be an instrument name (got " + show(value) + ")");
            map.byNote[size_t(note)] = instrumentNamed(value.get<std::string>(), where);
        }
        return map;
    } catch (const std::exception& e) {
        fail(path + ": " + e.what());
    }
}

bool savePadMap(const PadMap& map, const std::string& path) {
    try {
        // Keep the other keys of an existing file (e.g. "comment") and their order.
        detail::ojson root = detail::ojson::object();
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                auto existing = detail::ojson::parse(in, nullptr, false, true);
                if (existing.is_object()) root = std::move(existing);
            }
        }
        detail::ojson notes = detail::ojson::object();
        for (int n = 0; n < 128; ++n)
            if (map.byNote[size_t(n)] != Instrument::Unknown)
                notes[std::to_string(n)] = toString(map.byNote[size_t(n)]);
        root["hihatPedalCC"] = map.hihatPedalCC;
        root["notes"] = std::move(notes);

        const fs::path target(path);
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        const fs::path tmp = target.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << detail::dump(root, 2) << '\n';
            out.close();
            if (!out) {
                fs::remove(tmp, ec);
                return false;
            }
        }
        fs::rename(tmp, target, ec);  // atomic replace
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace drums
