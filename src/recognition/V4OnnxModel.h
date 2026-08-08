#pragma once

#include "V4ProbabilityModel.h"

#include <filesystem>
#include <memory>

namespace EchoRadar {

class V4OnnxModel final : public V4ProbabilityModel {
public:
    V4OnnxModel(const std::filesystem::path& modelPath,
                size_t inputFrames = 128,
                size_t inputBins = 64,
                size_t inputChannels = 5);
    ~V4OnnxModel() override;

    V4OnnxModel(const V4OnnxModel&) = delete;
    V4OnnxModel& operator=(const V4OnnxModel&) = delete;

    bool IsLoaded() const;
    const std::string& LoadError() const;
    size_t InputFrames() const override { return m_inputFrames; }
    size_t InputBins() const override { return m_inputBins; }
    size_t InputChannels() const override { return m_inputChannels; }
    bool Predict(std::span<const float> features,
                 V4ModelOutput& output,
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
