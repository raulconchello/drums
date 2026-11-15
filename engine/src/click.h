// Synthesized metronome click. Waveforms are computed once in prepare(); trigger() and renderAdd()
// are real-time safe. A click's first sample is its loudest, so its onset is exactly the trigger frame.
#pragma once

#include <array>
#include <vector>

namespace drums::engine {

enum class ClickKind : int { Accent = 0, Beat = 1, Subdivision = 2 };

class ClickSynth {
public:
    void prepare(double sampleRate);

    // Starts a click `offset` frames into the next renderAdd() block.
    void trigger(ClickKind kind, int offset) noexcept;
    // Adds `frames` of mono click signal to out.
    void renderAdd(float* out, int frames) noexcept;

    // Peak level of each kind before the click volume (accent 0.5 = -6 dBFS).
    static constexpr std::array<float, 3> kLevels = {0.5f, 0.36f, 0.2f};

private:
    struct Voice {
        int kind = 0;
        int position = 0;  // next sample index into the waveform
        int delay = 0;     // frames to wait in the current block
        bool active = false;
    };
    std::array<std::vector<float>, 3> waves_;
    std::array<Voice, 16> voices_{};
};

}  // namespace drums::engine
