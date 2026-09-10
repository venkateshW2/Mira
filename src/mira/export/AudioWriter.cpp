#include "AudioWriter.h"

#include <cstdint>
#include <fstream>

namespace mira {

namespace {

void writeU32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void writeU16(std::ofstream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }

} // namespace

bool writeWavFile(const std::string& path, const std::vector<float>& left,
                   const std::vector<float>& right, int sampleRate, bool stereo) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    const int numChannels = stereo ? 2 : 1;
    const uint32_t numFrames = static_cast<uint32_t>(left.size());
    const uint32_t bytesPerSample = 4; // 32-bit float
    const uint32_t dataBytes = numFrames * static_cast<uint32_t>(numChannels) * bytesPerSample;
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * numChannels * bytesPerSample;
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * bytesPerSample);

    out.write("RIFF", 4);
    writeU32(out, 36 + dataBytes);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    writeU32(out, 16); // fmt chunk size
    writeU16(out, 3);  // format tag 3 == IEEE float (WAVE_FORMAT_IEEE_FLOAT)
    writeU16(out, static_cast<uint16_t>(numChannels));
    writeU32(out, static_cast<uint32_t>(sampleRate));
    writeU32(out, byteRate);
    writeU16(out, blockAlign);
    writeU16(out, 32); // bits per sample

    out.write("data", 4);
    writeU32(out, dataBytes);

    if (stereo) {
        for (uint32_t i = 0; i < numFrames; ++i) {
            out.write(reinterpret_cast<const char*>(&left[i]), 4);
            out.write(reinterpret_cast<const char*>(&right[i]), 4);
        }
    } else if (!left.empty()) {
        out.write(reinterpret_cast<const char*>(left.data()), dataBytes);
    }

    return out.good();
}

} // namespace mira
