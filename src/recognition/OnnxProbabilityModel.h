#pragma once

#include "ProbabilityModel.h"

#include <filesystem>
#include <memory>

namespace EchoRadar {

class OnnxProbabilityModel final : public ProbabilityModel {
public:
    OnnxProbabilityModel(const std::filesystem::path& modelPath,
                         size_t inputFrames,
                         size_t inputBins,
                         size_t inputChannels = 1);
    ~OnnxProbabilityModel() override;

    OnnxProbabilityModel(const OnnxProbabilityModel&) = delete;
    OnnxProbabilityModel& operator=(const OnnxProbabilityModel&) = delete;

    bool IsLoaded() const;
    const std::string& LoadError() const;
    size_t InputFrames() const override { return m_inputFrames; }
    size_t InputBins() const override { return m_inputBins; }
    size_t InputChannels() const override { return m_inputChannels; }
    bool Predict(std::span<const float> logMel,
                 std::array<float, kSoundClassCount>& probabilities,
                 std::string* error = nullptr) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    size_t m_inputFrames;
    size_t m_inputBins;
    size_t m_inputChannels;
    std::string m_error;
};

} // namespace EchoRadar
