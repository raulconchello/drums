#include "drum_sampler.h"

#include "log.h"
#include "sfz_router.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace drums::engine {

namespace {
constexpr uintmax_t kRamBasedLimitBytes = 300u * 1024u * 1024u;
}

std::unique_ptr<DrumSampler> DrumSampler::create(const KitConfig& kit, double sampleRate, int maxBlock,
                                                 bool freewheel, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<DrumSampler> s(new DrumSampler());
    s->kit_ = kit;
    s->maxBlock_ = maxBlock;
    s->storage_.assign(size_t(2 * kNumOutputs) * size_t(maxBlock), 0.0f);
    for (size_t c = 0; c < s->channels_.size(); ++c) s->channels_[c] = s->storage_.data() + c * size_t(maxBlock);

    s->synth_.setSampleRate(float(sampleRate));
    s->synth_.setSamplesPerBlock(maxBlock);
    s->synth_.setNumVoices(96);
    if (freewheel) s->synth_.enableFreeWheeling();

    // Route kit note -> output of the (first) instrument using it.
    std::array<int, 128> outputForNote{};
    for (size_t i = 0; i < kNumInstruments; ++i) {
        const int note = kit.notes[i];
        if (note >= 0 && note < 128 && outputForNote[size_t(note)] == 0) outputForNote[size_t(note)] = int(1 + i);
    }

    // Small kits (all kits fetched by kits/fetch-kits.sh) are kept entirely in RAM: no disk
    // streaming and no sfizz loader jobs while playing. Big multi-GB kits stream from disk.
    std::error_code ec;
    uintmax_t kitBytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(kit.sfzPath).parent_path(), ec))
        if (entry.is_regular_file(ec)) kitBytes += entry.file_size(ec);
    const bool ramBased = kitBytes < kRamBasedLimitBytes;

    RoutedSfz routed;
    std::string routeError;
    if (routeSfzByKey(kit.sfzPath, outputForNote, ramBased, routed, &routeError) &&
        s->synth_.loadSfzString(kit.sfzPath, routed.text)) {
        s->routed_ = true;
    } else {
        logWarn("kit '%s': per-instrument routing failed (%s); loading it unrouted (no per-instrument volume)",
                kit.name.c_str(), routeError.c_str());
        if (!s->synth_.loadSfzFile(kit.sfzPath)) {
            if (error) *error = "sfizz could not load " + kit.sfzPath;
            return nullptr;
        }
    }
    if (s->synth_.getNumRegions() == 0) {
        if (error) *error = "no playable regions in " + kit.sfzPath;
        return nullptr;
    }
    const auto& unknown = s->synth_.getUnknownOpcodes();
    if (!unknown.empty()) {
        std::string list;
        for (const auto& op : unknown) list += (list.empty() ? "" : ", ") + op;
        logInfo("kit '%s': opcodes ignored by sfizz: %s", kit.name.c_str(), list.c_str());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logInfo("kit '%s': %d regions (%d routed to instrument outputs, %d split), %zu samples %s, %.0f ms",
            kit.name.c_str(), s->synth_.getNumRegions(), routed.routed, routed.split,
            s->synth_.getNumPreloadedSamples(), ramBased ? "fully in RAM" : "preloaded (streaming)", ms);
    return s;
}

void DrumSampler::noteOn(int delay, Instrument instrument, int velocity) noexcept {
    const int note = kitNote(instrument);
    if (note >= 0) synth_.noteOn(delay, note, std::clamp(velocity, 1, 127));
}

void DrumSampler::noteOff(int delay, Instrument instrument) noexcept {
    const int note = kitNote(instrument);
    if (note >= 0) synth_.noteOff(delay, note, 0);
}

void DrumSampler::hihatPedal(int delay, int value) noexcept {
    synth_.cc(delay, kit_.hihatCC, std::clamp(value, 0, 127));
}

void DrumSampler::renderAdd(int frames, float* outL, float* outR, const float* gainFrom,
                            const float* gainTo) noexcept {
    frames = std::min(frames, maxBlock_);
    if (frames <= 0) return;
    synth_.renderBlock(channels_.data(), size_t(frames), kNumOutputs);
    const float inv = 1.0f / float(frames);
    for (int o = 0; o < kNumOutputs; ++o) {
        const float g0 = gainFrom[o], g1 = gainTo[o];
        if (g0 == 0.0f && g1 == 0.0f) continue;
        const float* l = channels_[size_t(2 * o)];
        const float* r = channels_[size_t(2 * o + 1)];
        const float step = (g1 - g0) * inv;
        float g = g0;
        for (int f = 0; f < frames; ++f) {
            outL[f] += g * l[f];
            outR[f] += g * r[f];
            g += step;
        }
    }
}

}  // namespace drums::engine
