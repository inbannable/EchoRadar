#pragma once

#include "SoundRecognitionTypes.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace EchoRadar {

class ProbabilityModel {
public:
    virtual ~ProbabilityModel() = default;
    virtual size_t InputFrames() const = 0;
    virtual size_t InputBins() const = 0;
    virtual size_t InputChannels() const { return 1; }
    virtual bool Predict(std::span<const float> logMel,
                         std::array<float, kSoundClassCount>& probabilities,
                         std::string* error = nullptr) = 0;
};

} // namespace EchoRadar
