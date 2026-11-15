// Rewrites an SFZ kit so each drum piece renders to its own sfizz stereo output (SFZ "output"
// opcode), which gives the mixer per-instrument faders while keeping the whole kit in ONE sfizz
// instance (so hi-hat open/closed choke groups, note_polyphony etc. keep working).
// Not real-time; uses sfizz's own parser so #include, #define and header inheritance behave
// exactly as when sfizz loads the file.
#pragma once

#include <array>
#include <string>

namespace drums::engine {

struct RoutedSfz {
    std::string text;      // self-contained SFZ (includes and defines resolved)
    int regions = 0;       // regions in the source
    int routed = 0;        // regions given a non-zero output
    int split = 0;         // regions spanning several routed keys, duplicated once per key
};

// outputForNote[n] = stereo output index for kit note n (0 = leave on the shared output 0).
// ramBased: also ask sfizz to keep whole samples in RAM (no disk streaming while playing).
// Returns false and fills *error when the file cannot be parsed.
bool routeSfzByKey(const std::string& sfzPath, const std::array<int, 128>& outputForNote, bool ramBased,
                   RoutedSfz& result, std::string* error);

}  // namespace drums::engine
