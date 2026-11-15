// One sfizz instance playing a drum kit, rendering each instrument to its own stereo output.
// Loading is not real-time (app thread); noteOn/noteOff/cc/renderAdd are real-time safe.
#pragma once

#include "kit.h"

#include <sfizz.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace drums::engine {

// sfizz stereo outputs: 0 = anything not routed to an instrument, 1 + instrumentIndex otherwise.
inline constexpr int kNumOutputs = 1 + int(kNumInstruments);

class DrumSampler {
public:
    // Builds the per-instrument routed SFZ and loads it. freewheel = offline rendering: sfizz waits
    // for sample streaming instead of playing silence. Returns nullptr and fills *error on failure.
    static std::unique_ptr<DrumSampler> create(const KitConfig& kit, double sampleRate, int maxBlock,
                                               bool freewheel, std::string* error);

    const KitConfig& kit() const { return kit_; }
    bool routed() const { return routed_; }  // false: per-instrument volume unavailable (fallback load)
    int kitNote(Instrument i) const { return i == Instrument::Unknown ? -1 : kit_.notes[instrumentIndex(i)]; }

    // ---- real-time ----
    void noteOn(int delay, Instrument instrument, int velocity) noexcept;
    void noteOff(int delay, Instrument instrument) noexcept;
    void hihatPedal(int delay, int value) noexcept;  // forwarded as the kit's hi-hat CC
    // Renders frames (<= maxBlock) and ADDS them to outL/outR. Output o is scaled by a gain ramped
    // linearly from gainFrom[o] to gainTo[o] across the block.
    void renderAdd(int frames, float* outL, float* outR, const float* gainFrom, const float* gainTo) noexcept;
    int activeVoices() const noexcept { return synth_.getNumActiveVoices(); }

private:
    DrumSampler() = default;

    KitConfig kit_;
    bool routed_ = false;
    int maxBlock_ = 0;
    sfz::Sfizz synth_;
    std::vector<float> storage_;
    std::array<float*, 2 * kNumOutputs> channels_{};
};

}  // namespace drums::engine
