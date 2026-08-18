#pragma once

#include "DirectionModelPackage.h"
#include "DirectionTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace EchoRadar {

/// Scene-level ONNX inference and class-wise Multi-ACCDOA post-processing.
class DirectionOnnxEngine {
public:
    static constexpr size_t kRawValueCount =
        DirectionModelPackage::kClassCount * DirectionModelPackage::kTrackCount *
        DirectionModelPackage::kCoordinateCount;
    using RawOutput = std::array<float, kRawValueCount>;

    explicit DirectionOnnxEngine(DirectionModelPackage package);
    ~DirectionOnnxEngine();

    DirectionOnnxEngine(const DirectionOnnxEngine&) = delete;
    DirectionOnnxEngine& operator=(const DirectionOnnxEngine&) = delete;

    bool IsLoaded() const;
    const std::string& LoadError() const { return m_error; }
    const DirectionModelPackage& Package() const { return m_package; }

    bool ExtractFeatures(std::span<const float> interleavedStereo,
                         std::vector<float>& features,
                         std::string* error = nullptr) const;

    DirectionSceneResult Predict(uint64_t sceneId,
                                 uint64_t streamGeneration,
                                 uint64_t anchorEventSample,
                                 uint64_t sceneStartSample,
                                 std::span<const float> interleavedStereo,
                                 uint8_t enabledClassMask,
                                 std::string* error = nullptr);

    static DirectionSceneResult PostProcess(
        const DirectionModelPackage& package,
        const RawOutput& output,
        uint8_t enabledClassMask,
        DirectionSceneResult result = {});

private:
    struct Impl;
    DirectionModelPackage m_package;
    std::unique_ptr<Impl> m_impl;
    std::string m_error;
};

} // namespace EchoRadar

