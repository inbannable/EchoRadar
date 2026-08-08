#include "V4OnnxModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

#ifdef ECHORADAR_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace EchoRadar {

struct V4OnnxModel::Impl {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "EchoRadarV4"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
#endif
};

V4OnnxModel::V4OnnxModel(const std::filesystem::path& modelPath,
                         size_t inputFrames,
                         size_t inputBins,
                         size_t inputChannels)
    : m_impl(std::make_unique<Impl>()),
      m_inputFrames(inputFrames),
      m_inputBins(inputBins),
      m_inputChannels(inputChannels) {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    try {
        m_impl->options.SetIntraOpNumThreads(1);
        m_impl->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        m_impl->session = std::make_unique<Ort::Session>(
            m_impl->environment, modelPath.wstring().c_str(), m_impl->options);
#else
        m_impl->session = std::make_unique<Ort::Session>(
            m_impl->environment, modelPath.string().c_str(), m_impl->options);
#endif
    } catch (const Ort::Exception& exception) {
        m_error = exception.what();
    }
#else
    (void)modelPath;
    m_error = "EchoRadar was built without ONNX Runtime; configure with "
              "-DECHORADAR_ENABLE_ONNX=ON and ONNXRUNTIME_ROOT";
#endif
}

V4OnnxModel::~V4OnnxModel() = default;

bool V4OnnxModel::IsLoaded() const {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    return m_impl && m_impl->session != nullptr;
#else
    return false;
#endif
}

const std::string& V4OnnxModel::LoadError() const {
    return m_error;
}

bool V4OnnxModel::Predict(std::span<const float> features,
                          V4ModelOutput& output,
                          std::string* error) {
    output = {};
    if (!IsLoaded()) {
        if (error) *error = m_error;
        return false;
    }
    if (features.size() != m_inputChannels * m_inputFrames * m_inputBins) {
        if (error) *error = "Unexpected V4 ONNX input size";
        return false;
    }
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    try {
        const std::array<int64_t, 4> inputShape{
            1,
            static_cast<int64_t>(m_inputChannels),
            static_cast<int64_t>(m_inputFrames),
            static_cast<int64_t>(m_inputBins),
        };
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, const_cast<float*>(features.data()), features.size(),
            inputShape.data(), inputShape.size());
        const char* inputNames[] = {"features"};
        const char* outputNames[] = {"onset_probabilities", "source_probabilities"};
        auto outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 2);
        if (outputs.size() != 2 || !outputs[0].IsTensor() || !outputs[1].IsTensor()) {
            throw std::runtime_error("V4 ONNX outputs are not two tensors");
        }
        const auto onsetInfo = outputs[0].GetTensorTypeAndShapeInfo();
        const auto sourceInfo = outputs[1].GetTensorTypeAndShapeInfo();
        const auto onsetShape = onsetInfo.GetShape();
        const auto sourceShape = sourceInfo.GetShape();
        if (onsetShape != std::vector<int64_t>{1, static_cast<int64_t>(m_inputFrames), 2} ||
            sourceShape != std::vector<int64_t>{1, static_cast<int64_t>(m_inputFrames), 2, 3}) {
            throw std::runtime_error("V4 ONNX output shapes are incompatible");
        }
        const float* onset = outputs[0].GetTensorData<float>() +
            (m_inputFrames - 1) * kV4SoundClassCount;
        const float* source = outputs[1].GetTensorData<float>() +
            (m_inputFrames - 1) * kV4SoundClassCount * kV4SourceCount;
        for (size_t classIndex = 0; classIndex < kV4SoundClassCount; ++classIndex) {
            if (!std::isfinite(onset[classIndex])) {
                throw std::runtime_error("V4 ONNX onset output contains a non-finite value");
            }
            output.onsetProbabilities[classIndex] =
                std::clamp(onset[classIndex], 0.0f, 1.0f);
            for (size_t sourceIndex = 0; sourceIndex < kV4SourceCount; ++sourceIndex) {
                if (!std::isfinite(source[classIndex * kV4SourceCount + sourceIndex])) {
                    throw std::runtime_error("V4 ONNX source output contains a non-finite value");
                }
                output.sourceProbabilities[classIndex][sourceIndex] = std::clamp(
                    source[classIndex * kV4SourceCount + sourceIndex], 0.0f, 1.0f);
            }
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
#else
    return false;
#endif
}

} // namespace EchoRadar
