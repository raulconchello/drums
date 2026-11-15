#include "drums/midi_recorder.h"

#include "test_util.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace drums;

namespace {

// ---- a tiny Standard MIDI File reader, enough for format 0 / 1 with one track ----

struct SmfEvent {
    int64_t tick = 0;              // absolute
    uint8_t status = 0;            // 0xFF for meta
    uint8_t metaType = 0;          // meta only
    std::vector<uint8_t> data;     // channel data bytes, or meta payload
};

struct Smf {
    int format = -1;
    int tracks = 0;
    int division = 0;
    std::vector<SmfEvent> events;  // first track
};

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    uint8_t u8() {
        if (pos >= b.size()) throw std::runtime_error("unexpected end of file");
        return b[pos++];
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8) | u8();
        return v;
    }
    uint16_t u16() { return uint16_t((u8() << 8) | u8()); }
    uint32_t varLen() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            uint8_t c = u8();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        throw std::runtime_error("variable-length quantity longer than 4 bytes");
    }
    std::string tag() {
        std::string s;
        for (int k = 0; k < 4; ++k) s += char(u8());
        return s;
    }
};

Smf parseSmf(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    Smf smf;
    if (r.tag() != "MThd") throw std::runtime_error("no MThd");
    if (r.u32() != 6) throw std::runtime_error("bad header length");
    smf.format = r.u16();
    smf.tracks = r.u16();
    smf.division = r.u16();
    if (r.tag() != "MTrk") throw std::runtime_error("no MTrk");
    const uint32_t length = r.u32();
    const size_t end = r.pos + length;
    if (end != bytes.size()) throw std::runtime_error("track length does not match the file size");
    int64_t tick = 0;
    uint8_t running = 0;
    while (r.pos < end) {
        tick += r.varLen();
        SmfEvent e;
        e.tick = tick;
        uint8_t status = r.u8();
        if (status == 0xFF) {
            e.status = 0xFF;
            e.metaType = r.u8();
            const uint32_t n = r.varLen();
            for (uint32_t k = 0; k < n; ++k) e.data.push_back(r.u8());
        } else {
            uint8_t first;
            if (status & 0x80) {
                running = status;
                first = r.u8();
            } else {  // running status
                first = status;
                status = running;
            }
            e.status = status;
            e.data.push_back(first);
            const uint8_t kind = status & 0xF0;
            if (kind != 0xC0 && kind != 0xD0) e.data.push_back(r.u8());
        }
        smf.events.push_back(std::move(e));
    }
    return smf;
}

std::vector<uint8_t> readBytes(const std::filesystem::path& p) {
    const std::string s = test::readFile(p);
    return {s.begin(), s.end()};
}

struct Ev {
    int64_t tick;
    uint8_t status, d1, d2;
    bool operator==(const Ev&) const = default;
};

std::vector<Ev> channelEvents(const Smf& smf) {
    std::vector<Ev> out;
    for (auto& e : smf.events)
        if (e.status != 0xFF) out.push_back({e.tick, e.status, e.data[0], e.data.size() > 1 ? e.data[1] : uint8_t(0)});
    return out;
}

}  // namespace

TEST_CASE("midi recorder: writes a format 0 SMF with tempo, time signature and channel 10 events") {
    auto dir = test::tempDir("midi");
    const auto path = dir / "recordings" / "deep" / "take.mid";  // parent dirs do not exist yet

    MidiRecorder rec;
    CHECK_FALSE(rec.isRecording());
    rec.noteOn(36, 100, 0);  // not recording: ignored
    // 12/8 at 60 dotted-quarter bpm = 90 quarter-note bpm: 0.72 ticks per ms.
    rec.start(60.0 * 720 / 480, {12, 8});
    CHECK(rec.isRecording());
    rec.noteOn(36, 100, 0);
    rec.noteOff(36, 50);
    rec.noteOn(38, 90, 1000);
    rec.controlChange(4, 64, 1000);
    rec.noteOff(38, 1100);
    rec.noteOn(42, 80, 2000);           // never released: closed 60 ticks later
    rec.noteOn(46, 127, 100000);        // 72000 ticks: a 3-byte delta
    rec.noteOff(46, 100001.2);          // rounds to 72001
    rec.noteOn(999, 100, 5);            // invalid note: ignored
    REQUIRE(rec.stopAndSave(path.string()));
    CHECK_FALSE(rec.isRecording());

    const Smf smf = parseSmf(readBytes(path));
    CHECK(smf.format == 0);
    CHECK(smf.tracks == 1);
    CHECK(smf.division == 480);

    // Meta events at tick 0: time signature, tempo; end of track last.
    REQUIRE(smf.events.size() >= 3);
    const SmfEvent& ts = smf.events[0];
    CHECK(ts.status == 0xFF);
    CHECK(ts.metaType == 0x58);
    CHECK(ts.tick == 0);
    CHECK(ts.data == std::vector<uint8_t>{12, 3, 36, 8});  // 12/8, click every dotted quarter
    const SmfEvent& tempo = smf.events[1];
    CHECK(tempo.metaType == 0x51);
    CHECK(tempo.tick == 0);
    REQUIRE(tempo.data.size() == 3);
    const uint32_t usPerQuarter = (tempo.data[0] << 16) | (tempo.data[1] << 8) | tempo.data[2];
    CHECK(usPerQuarter == 666667);  // 60e6 / 90
    const SmfEvent& eot = smf.events.back();
    CHECK(eot.status == 0xFF);
    CHECK(eot.metaType == 0x2F);
    CHECK(eot.data.empty());

    const std::vector<Ev> expected = {
        {0, 0x99, 36, 100},   {36, 0x89, 36, 64},  {720, 0x99, 38, 90},     {720, 0xB9, 4, 64},
        {792, 0x89, 38, 64},  {1440, 0x99, 42, 80}, {1500, 0x89, 42, 64},   {72000, 0x99, 46, 127},
        {72001, 0x89, 46, 64},
    };
    CHECK(channelEvents(smf) == expected);
    CHECK(eot.tick == 72001);
}

TEST_CASE("midi recorder: 4/4 meta, sorting, repeated note-ons, empty take") {
    auto dir = test::tempDir("midi-44");
    MidiRecorder rec;
    rec.start(120, {4, 4});  // 0.96 ticks per ms
    rec.noteOn(38, 100, 500);
    rec.noteOn(36, 0, 200);   // velocity 0 = note-off
    rec.noteOn(36, 90, 100);  // out of order in time: sorted
    rec.noteOn(38, 110, 520); // 38 still down: a note-off is inserted first
    rec.noteOff(38, 600);
    rec.noteOn(40, 300, -5);  // velocity clamped, negative time clamped to 0
    REQUIRE(rec.stopAndSave((dir / "a.mid").string()));
    const Smf smf = parseSmf(readBytes(dir / "a.mid"));
    CHECK(smf.events[0].data == std::vector<uint8_t>{4, 2, 24, 8});
    const uint32_t us = (smf.events[1].data[0] << 16) | (smf.events[1].data[1] << 8) | smf.events[1].data[2];
    CHECK(us == 500000);
    const std::vector<Ev> expected = {
        {0, 0x99, 40, 127},  {96, 0x99, 36, 90}, {192, 0x89, 36, 64}, {480, 0x99, 38, 100},
        {499, 0x89, 38, 64}, {499, 0x99, 38, 110}, {576, 0x89, 38, 64}, {60, 0x89, 40, 64},
    };
    // The dangling note 40 is closed at tick 60: sorted into place.
    std::vector<Ev> sorted = expected;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Ev& a, const Ev& b) { return a.tick < b.tick; });
    CHECK(channelEvents(smf) == sorted);

    // An empty take is still a valid file.
    rec.start(100, {3, 4});
    REQUIRE(rec.stopAndSave((dir / "empty.mid").string()));
    const Smf empty = parseSmf(readBytes(dir / "empty.mid"));
    CHECK(empty.events.size() == 3);
    CHECK(channelEvents(empty).empty());

    // Nothing to save without a take.
    CHECK_FALSE(rec.stopAndSave((dir / "none.mid").string()));
}

TEST_CASE("midi recorder: IO error keeps the take, start discards it") {
    auto dir = test::tempDir("midi-io");
    MidiRecorder rec;
    rec.start(90, {12, 8});
    rec.noteOn(36, 100, 10);
    CHECK_FALSE(rec.stopAndSave("/proc/drums-no-such-dir/take.mid"));
    CHECK_FALSE(rec.isRecording());
    REQUIRE(rec.stopAndSave((dir / "retry.mid").string()));  // the kept take
    CHECK(channelEvents(parseSmf(readBytes(dir / "retry.mid"))).size() == 2);  // on + generated off

    rec.start(90, {12, 8});
    rec.noteOn(36, 100, 10);
    rec.start(90, {12, 8});  // discards the unsaved take
    rec.noteOn(38, 100, 10);
    REQUIRE(rec.stopAndSave((dir / "second.mid").string()));
    const auto events = channelEvents(parseSmf(readBytes(dir / "second.mid")));
    REQUIRE(events.size() == 2);
    CHECK(events[0].d1 == 38);
}
