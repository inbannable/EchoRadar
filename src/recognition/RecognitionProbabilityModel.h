#pragma once

#include "RecognitionTypes.h"

#include <cstddef>
#include <span>
#include <string>

namespace EchoRadar {

class RecognitionProbabilityModel {
public:
    virtual ~RecognitionProbabilityModel() = default;
    virtual size_t InputFrames() const = 0;
    virtual size_t InputBins() const = 0;
    virtual size_t InputChannels() const = 0;
    virtual bool Predict(std::span<const float> features,
                         RecognitionModelOutput& output,
                         std::string* error = nullptr) = 0;
};

} // namespace EchoRadar

