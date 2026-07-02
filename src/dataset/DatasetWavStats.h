#pragma once

// Minimal PCM16 stereo WAV reader used only for dataset quality checks
// (duration / peak / rms). Mirrors the writer used by the dataset recorder
// (RIFF/WAVE, fmt chunk, data chunk, 16-bit signed PCM, interleaved stereo).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace EchoRadar::detail {

struct WavInfo {
    bool ok{false};
    uint32_t sampleRate{0};
    uint16_t channels{0};
    uint64_t frameCount{0};
    float peak{0.0f};
    float rms{0.0f};
    double durationMs{0.0};
};

inline WavInfo ReadWavQuickStats(const std::string& path) {
    WavInfo info;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return info;
    }

    char riff[4];
    in.read(riff, 4);
    if (!in || std::strncmp(riff, "RIFF", 4) != 0) {
        return info;
    }
    uint32_t riffSize = 0;
    in.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
    char wave[4];
    in.read(wave, 4);
    if (!in || std::strncmp(wave, "WAVE", 4) != 0) {
        return info;
    }

    uint16_t bitsPerSample = 16;
    uint32_t dataBytes = 0;
    std::streampos dataPos{};

    while (in) {
        char chunkId[4];
        in.read(chunkId, 4);
        if (!in) break;
        uint32_t chunkSize = 0;
        in.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        if (!in) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0;
            in.read(reinterpret_cast<char*>(&audioFormat), sizeof(audioFormat));
            in.read(reinterpret_cast<char*>(&info.channels), sizeof(info.channels));
            in.read(reinterpret_cast<char*>(&info.sampleRate), sizeof(info.sampleRate));
            uint32_t byteRate = 0;
            in.read(reinterpret_cast<char*>(&byteRate), sizeof(byteRate));
            uint16_t blockAlign = 0;
            in.read(reinterpret_cast<char*>(&blockAlign), sizeof(blockAlign));
            in.read(reinterpret_cast<char*>(&bitsPerSample), sizeof(bitsPerSample));
            const std::streamoff consumed = 16;
            if (chunkSize > consumed) {
                in.seekg(static_cast<std::streamoff>(chunkSize) - consumed, std::ios::cur);
            }
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataBytes = chunkSize;
            dataPos = in.tellg();
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
    }

    if (dataBytes == 0 || info.channels == 0 || info.sampleRate == 0 || bitsPerSample != 16) {
        return info;
    }

    in.clear();
    in.seekg(dataPos);
    const size_t sampleCount = dataBytes / sizeof(int16_t);
    std::vector<int16_t> raw(sampleCount);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(dataBytes));

    double sumSq = 0.0;
    float peak = 0.0f;
    for (int16_t s : raw) {
        const float f = static_cast<float>(s) / 32768.0f;
        peak = std::max(peak, std::fabs(f));
        sumSq += static_cast<double>(f) * static_cast<double>(f);
    }

    info.frameCount = sampleCount / info.channels;
    info.peak = peak;
    info.rms = sampleCount > 0 ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(sampleCount))) : 0.0f;
    info.durationMs = info.sampleRate > 0
        ? (static_cast<double>(info.frameCount) / static_cast<double>(info.sampleRate)) * 1000.0
        : 0.0;
    info.ok = true;
    return info;
}

struct WavSamples {
    bool ok{false};
    uint32_t sampleRate{0};
    uint16_t channels{0};
    std::vector<float> interleaved; // range [-1, 1]
};

// Reads the entire PCM16 payload into memory as normalized floats. Used by
// Dataset Studio to render waveform/spectrogram previews. Not intended for
// large files -- dataset clips are ~400ms so this is cheap.
inline WavSamples ReadWavFullPcm16(const std::string& path) {
    WavSamples out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;

    char riff[4];
    in.read(riff, 4);
    if (!in || std::strncmp(riff, "RIFF", 4) != 0) return out;
    uint32_t riffSize = 0;
    in.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
    char wave[4];
    in.read(wave, 4);
    if (!in || std::strncmp(wave, "WAVE", 4) != 0) return out;

    uint16_t bitsPerSample = 16;
    uint32_t dataBytes = 0;
    std::streampos dataPos{};

    while (in) {
        char chunkId[4];
        in.read(chunkId, 4);
        if (!in) break;
        uint32_t chunkSize = 0;
        in.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        if (!in) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0;
            in.read(reinterpret_cast<char*>(&audioFormat), sizeof(audioFormat));
            in.read(reinterpret_cast<char*>(&out.channels), sizeof(out.channels));
            in.read(reinterpret_cast<char*>(&out.sampleRate), sizeof(out.sampleRate));
            uint32_t byteRate = 0;
            in.read(reinterpret_cast<char*>(&byteRate), sizeof(byteRate));
            uint16_t blockAlign = 0;
            in.read(reinterpret_cast<char*>(&blockAlign), sizeof(blockAlign));
            in.read(reinterpret_cast<char*>(&bitsPerSample), sizeof(bitsPerSample));
            const std::streamoff consumed = 16;
            if (chunkSize > consumed) {
                in.seekg(static_cast<std::streamoff>(chunkSize) - consumed, std::ios::cur);
            }
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataBytes = chunkSize;
            dataPos = in.tellg();
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
    }

    if (dataBytes == 0 || out.channels == 0 || out.sampleRate == 0 || bitsPerSample != 16) {
        return out;
    }

    in.clear();
    in.seekg(dataPos);
    const size_t sampleCount = dataBytes / sizeof(int16_t);
    std::vector<int16_t> raw(sampleCount);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(dataBytes));

    out.interleaved.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        out.interleaved[i] = static_cast<float>(raw[i]) / 32768.0f;
    }
    out.ok = true;
    return out;
}

// Cheap FNV-1a hash over the file bytes; good enough to flag byte-identical
// duplicate recordings without pulling in a real hashing library.
inline uint64_t HashFileFnv1a(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    uint64_t hash = 1469598103934665603ull;
    char buf[4096];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<uint8_t>(buf[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

} // namespace EchoRadar::detail
