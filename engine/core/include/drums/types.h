// Shared vocabulary for the engine. Header-only, no dependencies.
// CONTRACT FILE: changing anything here affects the audio engine, the core library and the UI protocol.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace drums {

enum class Instrument : uint8_t {
    Kick, Snare, SnareRim, HiHatClosed, HiHatOpen, HiHatPedal,
    Tom1, Tom2, Tom3, Crash, Ride, RideBell,
    Unknown
};

inline constexpr std::array<Instrument, 12> kInstruments = {
    Instrument::Kick, Instrument::Snare, Instrument::SnareRim,
    Instrument::HiHatClosed, Instrument::HiHatOpen, Instrument::HiHatPedal,
    Instrument::Tom1, Instrument::Tom2, Instrument::Tom3,
    Instrument::Crash, Instrument::Ride, Instrument::RideBell,
};

// Names used in JSON files and the WebSocket protocol.
constexpr std::string_view toString(Instrument i) {
    switch (i) {
        case Instrument::Kick: return "kick";
        case Instrument::Snare: return "snare";
        case Instrument::SnareRim: return "snareRim";
        case Instrument::HiHatClosed: return "hihatClosed";
        case Instrument::HiHatOpen: return "hihatOpen";
        case Instrument::HiHatPedal: return "hihatPedal";
        case Instrument::Tom1: return "tom1";
        case Instrument::Tom2: return "tom2";
        case Instrument::Tom3: return "tom3";
        case Instrument::Crash: return "crash";
        case Instrument::Ride: return "ride";
        case Instrument::RideBell: return "rideBell";
        case Instrument::Unknown: break;
    }
    return "unknown";
}

constexpr Instrument instrumentFromString(std::string_view s) {
    for (Instrument i : kInstruments)
        if (toString(i) == s) return i;
    return Instrument::Unknown;
}

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;
};

enum class Grade : uint8_t { OnTime, Early, Late, Extra, Free };

constexpr std::string_view toString(Grade g) {
    switch (g) {
        case Grade::OnTime: return "onTime";
        case Grade::Early: return "early";
        case Grade::Late: return "late";
        case Grade::Extra: return "extra";
        case Grade::Free: return "free";
    }
    return "free";
}

// MIDI note (0-127) -> instrument. Unknown = unmapped.
struct PadMap {
    std::array<Instrument, 128> byNote{};
    int hihatPedalCC = 4;
    PadMap() { byNote.fill(Instrument::Unknown); }
    Instrument operator[](int note) const {
        return (note >= 0 && note < 128) ? byNote[static_cast<size_t>(note)] : Instrument::Unknown;
    }
};

}  // namespace drums
