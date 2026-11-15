#include "wav_writer.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace drums::engine {
namespace {

void put32(std::ofstream& out, uint32_t v) {
    const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
    out.write(b, 4);
}
void put16(std::ofstream& out, uint16_t v) {
    const char b[2] = {char(v), char(v >> 8)};
    out.write(b, 2);
}

}  // namespace

bool writeWavFloatStereo(const std::string& path, int sampleRate, const std::vector<float>& left,
                         const std::vector<float>& right) {
    if (left.size() != right.size()) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t frames = uint32_t(left.size());
    const uint32_t dataBytes = frames * 2u * 4u;
    out.write("RIFF", 4);
    put32(out, 4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put32(out, 18);
    put16(out, 3);  // WAVE_FORMAT_IEEE_FLOAT
    put16(out, 2);
    put32(out, uint32_t(sampleRate));
    put32(out, uint32_t(sampleRate) * 8u);
    put16(out, 8);
    put16(out, 32);
    put16(out, 0);  // cbSize
    out.write("fact", 4);
    put32(out, 4);
    put32(out, frames);
    out.write("data", 4);
    put32(out, dataBytes);
    std::vector<float> interleaved(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        interleaved[2 * size_t(i)] = left[i];
        interleaved[2 * size_t(i) + 1] = right[i];
    }
    static_assert(sizeof(float) == 4);
    out.write(reinterpret_cast<const char*>(interleaved.data()), std::streamsize(interleaved.size() * 4));
    return bool(out);
}

}  // namespace drums::engine
