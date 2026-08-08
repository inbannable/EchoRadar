#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace EchoRadar {

struct V4ModelPackage {
    std::filesystem::path directory;
    std::filesystem::path modelPath;
    std::string modelVersion;
    std::string modelSha256;
    std::string preprocessingVersion;
    uint32_t sampleRate{48000};
    uint32_t fftSize{1024};
    uint32_t hopSize{240};
    uint32_t melBins{64};
    uint32_t contextFrames{128};
    uint32_t inputChannels{5};
    uint32_t inferenceStrideFrames{2};
    float pcenSmoothing{0.025f};
    float pcenAlpha{0.98f};
    float pcenDelta{2.0f};
    float pcenRoot{0.5f};
    float pcenEpsilon{1e-6f};
    float sceneActivityCutoff{0.5f};
    float selfSuppressionThreshold{0.95f};
    uint32_t peakLookaheadFrames{2};
    uint64_t pulseSamples{2400};
    std::array<float, 2> quietThresholds{};
    std::array<float, 2> busyThresholds{};
    std::array<uint64_t, 2> minimumSpacingSamples{};
    std::array<uint32_t, 2> onsetOffsetSamples{};

    static bool Load(const std::filesystem::path& directory,
                     V4ModelPackage& package,
                     std::string* error = nullptr);
};

} // namespace EchoRadar
