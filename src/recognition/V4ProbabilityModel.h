#pragma once

#include "V4RecognitionTypes.h"

#include <cstddef>
#include <span>
#include <string>

namespace EchoRadar {

class V4ProbabilityModel {
public:
    virtual ~V4ProbabilityModel() = default;
    virtual size_t InputFrames() const = 0;
    virtual size_t InputBins() const = 0;
    virtual size_t InputChannels() const = 0;
    virtual bool Predict(std::span<const float> features,
                         V4ModelOutput& output,
                         std::string* error = nullptr) = 0;
};

} // namespace EchoRadar
