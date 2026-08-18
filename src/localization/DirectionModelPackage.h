#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace EchoRadar {

struct DirectionUncertaintyPoint {
    float confidence{0.0f};
    float p90AngularErrorDegrees{180.0f};
};

struct DirectionModelPackage {
    static constexpr size_t kClassCount = 2;
    static constexpr size_t kTrackCount = 3;
    static constexpr size_t kCoordinateCount = 3;
    static constexpr size_t kMaximumUncertaintyPoints = 16;

    std::filesystem::path directory;
    std::filesystem::path modelPath;
    std::string modelVersion;
    std::string modelSha256;
    std::string preprocessingVersion;
    uint32_t sampleRate{48000};
    uint32_t fftSize{1024};
    uint32_t hopSize{240};
    uint32_t melBins{64};
    uint32_t contextFrames{48};
    uint32_t contextSamples{12304};
    uint32_t inputChannels{5};
    float elevationMinDegrees{-60.0f};
    float elevationMaxDegrees{60.0f};
    std::array<float, kClassCount> activityThresholds{}; // gunshot, footstep
    float duplicateMergeDegrees{7.5f};
    float minimumTrainingSeparationDegrees{15.0f};
    uint32_t maximumSources{3};
    float pcenSmoothing{0.025f};
    float pcenAlpha{0.98f};
    float pcenDelta{2.0f};
    float pcenRoot{0.5f};
    float pcenEpsilon{1.0e-6f};
    uint32_t uncertaintyCount{0};
    std::array<DirectionUncertaintyPoint, kMaximumUncertaintyPoints> uncertainty{};

    float UncertaintyDegrees(float confidence) const;

    static bool Load(const std::filesystem::path& directory,
                     DirectionModelPackage& package,
                     std::string* error = nullptr);
};

} // namespace EchoRadar
