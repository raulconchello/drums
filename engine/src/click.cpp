#include "click.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace drums::engine {

void ClickSynth::prepare(double sampleRate) {
    struct Spec {
        double freq;     // Hz
        double decayMs;  // exponential time constant
        double lengthMs;
    };
    // Accent: bright and longer; beat: mid; subdivision: lower, short and quiet.
    const std::array<Spec, 3> specs = {{{1760.0, 9.0, 45.0}, {1320.0, 7.0, 35.0}, {990.0, 4.0, 22.0}}};
    for (size_t k = 0; k < specs.size(); ++k) {
        const Spec& s = specs[k];
        const int length = int(std::lround(s.lengthMs * sampleRate / 1000.0));
        const int fade = std::max(1, int(sampleRate * 0.003));  // 3 ms fade-out to reach exact silence
        auto& w = waves_[k];
        w.assign(size_t(length), 0.0f);
        for (int n = 0; n < length; ++n) {
            const double t = n / sampleRate;
            const double env = std::exp(-t * 1000.0 / s.decayMs);
            const double phase = 2.0 * std::numbers::pi * s.freq * t;
            // Cosine start: full-level first sample = crisp, sample-exact onset. Plus a soft octave.
            double v = 0.8 * std::cos(phase) + 0.2 * std::cos(2.0 * phase);
            double tail = n >= length - fade ? double(length - n) / fade : 1.0;
            w[size_t(n)] = float(kLevels[k] * env * tail * v);
        }
    }
    for (auto& v : voices_) v.active = false;
}

void ClickSynth::trigger(ClickKind kind, int offset) noexcept {
    // Reuse a free voice, else steal the oldest (largest position).
    Voice* target = nullptr;
    for (auto& v : voices_) {
        if (!v.active) {
            target = &v;
            break;
        }
    }
    if (!target) {
        target = &voices_[0];
        for (auto& v : voices_)
            if (v.position > target->position) target = &v;
    }
    target->kind = int(kind);
    target->position = 0;
    target->delay = std::max(0, offset);
    target->active = true;
}

void ClickSynth::renderAdd(float* out, int frames) noexcept {
    for (auto& v : voices_) {
        if (!v.active) continue;
        const auto& w = waves_[size_t(v.kind)];
        const int start = std::min(v.delay, frames);
        v.delay -= start;
        const int count = std::min(frames - start, int(w.size()) - v.position);
        for (int i = 0; i < count; ++i) out[start + i] += w[size_t(v.position + i)];
        v.position += std::max(count, 0);
        if (v.delay == 0 && v.position >= int(w.size())) v.active = false;
    }
}

}  // namespace drums::engine
