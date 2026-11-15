// Drum kit descriptions (kits/<name>/kit.json). Not real-time.
#pragma once

#include "drums/types.h"

#include <array>
#include <string>
#include <vector>

namespace drums::engine {

inline constexpr size_t kNumInstruments = kInstruments.size();

inline constexpr size_t instrumentIndex(Instrument i) { return static_cast<size_t>(i); }

struct KitConfig {
    std::string name;
    std::string jsonPath;  // absolute path of kit.json
    std::string sfzPath;   // absolute path of the .sfz to load
    std::array<int, kNumInstruments> notes{};  // instrument -> kit (SFZ) note, -1 = the kit lacks it
    int hihatCC = 4;       // CC number the kit's SFZ uses for the hi-hat pedal
    std::string license;
    std::string source;

    KitConfig() { notes.fill(-1); }
};

// Parses a kit.json. Throws std::runtime_error with a readable message.
KitConfig loadKitConfig(const std::string& jsonPath);

struct KitListEntry {
    std::string name;
    std::string path;  // as shown to the UI: relative to root when inside it
};

// Scans <root>/kits/*/kit.json, sorted by folder name. Unreadable kits are skipped (logged).
std::vector<KitListEntry> scanKits(const std::string& root);

// Resolves a user-supplied path: absolute stays, relative is taken from root.
std::string resolvePath(const std::string& root, const std::string& path);
// Path relative to root when it lies inside root, else the absolute path.
std::string displayPath(const std::string& root, const std::string& path);

// The kit used when none is given: kits/avl-black-pearl, else the first kit found. "" if none.
std::string defaultKitPath(const std::string& root);

}  // namespace drums::engine
