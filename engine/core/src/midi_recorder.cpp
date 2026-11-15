#include "drums/midi_recorder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

namespace drums {
namespace {

constexpr int kPpq = 480;
constexpr uint8_t kChannel = 9;           // MIDI channel 10 (drums), 0-based
constexpr int64_t kDanglingNoteTicks = 60;  // a note-on without note-off is closed after a 1/32

struct Event {
    double timeMs = 0;
    uint8_t status = 0;  // 0x90 / 0x80 / 0xB0 (channel added when writing)
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

struct TickEvent {
    int64_t tick = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

void putVarLen(std::vector<uint8_t>& out, uint32_t value) {
    value = std::min<uint32_t>(value, 0x0FFFFFFF);  // SMF limit (4 bytes)
    uint8_t bytes[4];
    int n = 0;
    do {
        bytes[n++] = value & 0x7F;
        value >>= 7;
    } while (value != 0);
    while (n > 0) {
        --n;
        out.push_back(static_cast<uint8_t>(bytes[n] | (n > 0 ? 0x80 : 0)));
    }
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint8_t clamp7(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 127)); }

}  // namespace

struct MidiRecorder::Impl {
    mutable std::mutex mutex;
    bool recording = false;
    bool hasTake = false;
    double quarterNoteBpm = 120;
    TimeSignature timeSignature;
    std::vector<Event> events;

    void add(uint8_t status, int data1, int data2, double timeMs) {
        std::lock_guard lock(mutex);
        if (!recording) return;
        if (!std::isfinite(timeMs)) return;
        events.push_back({std::max(0.0, timeMs), status, clamp7(data1), clamp7(data2)});
    }

    std::vector<uint8_t> encode() const {
        // Absolute ticks, rounded per event (no accumulated drift).
        const double ticksPerMs = kPpq * quarterNoteBpm / 60000.0;
        std::vector<TickEvent> timed;
        timed.reserve(events.size() + 8);
        for (const Event& e : events)
            timed.push_back({std::llround(e.timeMs * ticksPerMs), e.status, e.data1, e.data2});
        std::stable_sort(timed.begin(), timed.end(),
                         [](const TickEvent& a, const TickEvent& b) { return a.tick < b.tick; });

        // Every note-on gets a note-off: before a repeated note-on of the same note, or a 1/32 later.
        std::vector<TickEvent> track;
        track.reserve(timed.size() * 2);
        std::array<bool, 128> open{};
        std::array<int64_t, 128> openedAt{};
        for (const TickEvent& e : timed) {
            if (e.status == 0x90) {
                if (open[e.data1]) track.push_back({e.tick, 0x80, e.data1, 0x40});
                open[e.data1] = true;
                openedAt[e.data1] = e.tick;
            } else if (e.status == 0x80) {
                open[e.data1] = false;
            }
            track.push_back(e);
        }
        for (int n = 0; n < 128; ++n)
            if (open[size_t(n)])
                track.push_back({openedAt[size_t(n)] + kDanglingNoteTicks, 0x80, uint8_t(n), 0x40});
        std::stable_sort(track.begin(), track.end(),
                         [](const TickEvent& a, const TickEvent& b) { return a.tick < b.tick; });

        std::vector<uint8_t> data;
        // Time signature: nn dd cc bb (dd = log2(denominator), cc = MIDI clocks per beat click).
        const int num = std::clamp(timeSignature.numerator, 1, 255);
        const int den = std::clamp(timeSignature.denominator, 1, 64);
        int log2den = 0;
        while ((1 << (log2den + 1)) <= den) ++log2den;
        const bool compound = den >= 8 && num > 3 && num % 3 == 0;
        const int clocksPerClick = std::max(1, (compound ? 3 : 1) * 96 / (1 << log2den));
        putVarLen(data, 0);
        data.insert(data.end(), {0xFF, 0x58, 0x04, uint8_t(num), uint8_t(log2den),
                                 uint8_t(std::min(clocksPerClick, 255)), 0x08});
        // Tempo: microseconds per quarter note.
        const double bpm = quarterNoteBpm > 0 ? quarterNoteBpm : 120.0;
        const auto usPerQuarter = static_cast<uint32_t>(std::clamp<long long>(std::llround(60000000.0 / bpm), 1, 0xFFFFFF));
        putVarLen(data, 0);
        data.insert(data.end(), {0xFF, 0x51, 0x03, uint8_t(usPerQuarter >> 16), uint8_t(usPerQuarter >> 8),
                                 uint8_t(usPerQuarter)});

        int64_t previous = 0;
        for (const TickEvent& e : track) {
            putVarLen(data, static_cast<uint32_t>(std::clamp<int64_t>(e.tick - previous, 0, 0x0FFFFFFF)));
            previous = e.tick;
            data.push_back(static_cast<uint8_t>(e.status | kChannel));
            data.push_back(e.data1);
            data.push_back(e.data2);
        }
        putVarLen(data, 0);
        data.insert(data.end(), {0xFF, 0x2F, 0x00});  // end of track

        std::vector<uint8_t> file;
        file.insert(file.end(), {'M', 'T', 'h', 'd'});
        putU32(file, 6);
        putU16(file, 0);  // format 0
        putU16(file, 1);  // one track
        putU16(file, kPpq);
        file.insert(file.end(), {'M', 'T', 'r', 'k'});
        putU32(file, static_cast<uint32_t>(data.size()));
        file.insert(file.end(), data.begin(), data.end());
        return file;
    }
};

MidiRecorder::MidiRecorder() : impl_(std::make_unique<Impl>()) {}
MidiRecorder::~MidiRecorder() = default;

void MidiRecorder::start(double quarterNoteBpm, TimeSignature timeSignature) {
    std::lock_guard lock(impl_->mutex);
    impl_->recording = true;
    impl_->hasTake = true;
    impl_->quarterNoteBpm = (std::isfinite(quarterNoteBpm) && quarterNoteBpm > 0) ? quarterNoteBpm : 120.0;
    impl_->timeSignature = timeSignature;
    impl_->events.clear();
}

bool MidiRecorder::isRecording() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->recording;
}

void MidiRecorder::noteOn(int note, int velocity, double timeMs) {
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {  // note-on with velocity 0 means note-off
        noteOff(note, timeMs);
        return;
    }
    impl_->add(0x90, note, std::max(1, velocity), timeMs);
}

void MidiRecorder::noteOff(int note, double timeMs) {
    if (note < 0 || note > 127) return;
    impl_->add(0x80, note, 0x40, timeMs);
}

void MidiRecorder::controlChange(int controller, int value, double timeMs) {
    if (controller < 0 || controller > 127) return;
    impl_->add(0xB0, controller, value, timeMs);
}

bool MidiRecorder::stopAndSave(const std::string& path) {
    std::vector<uint8_t> bytes;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->recording = false;
        if (!impl_->hasTake) return false;
        bytes = impl_->encode();
    }
    try {
        namespace fs = std::filesystem;
        const fs::path target(path);
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) return false;
    } catch (...) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->recording) {  // a new take may have started meanwhile; keep that one
        impl_->hasTake = false;
        impl_->events.clear();
    }
    return true;
}

}  // namespace drums
