// Writes interleaved 32-bit float WAV files (offline render, tests). Not real-time.
#pragma once

#include <string>
#include <vector>

namespace drums::engine {

// left/right must have the same length. Returns false on IO error.
bool writeWavFloatStereo(const std::string& path, int sampleRate, const std::vector<float>& left,
                         const std::vector<float>& right);

}  // namespace drums::engine
