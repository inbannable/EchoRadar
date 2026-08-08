#pragma once

#include "EventPostProcessor.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace EchoRadar {

struct ModelPackage {
    std::filesystem::path directory;
    std::filesystem::path modelPath;
    std::string modelVersion;
    std::string modelSha256;
    std::string preprocessingVersion;
    uint32_t sampleRate{48000};
    uint32_t fftSize{1024};
    uint32_t hopSize{512};
    uint32_t melBins{64};
    uint32_t contextFrames{96};
    uint32_t inferenceStrideFrames{5};
    uint32_t inputChannels{1};
    float pcenSmoothing{0.025f};
    float pcenAlpha{0.98f};
    float pcenDelta{2.0f};
    float pcenRoot{0.5f};
    float pcenEpsilon{1e-6f};
    EventPostProcessor::Config postProcessing;

    static bool Load(const std::filesystem::path& directory,
                     ModelPackage& package,
                     std::string* error = nullptr);
};

} // namespace EchoRadar
