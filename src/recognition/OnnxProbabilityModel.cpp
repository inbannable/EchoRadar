#include "OnnxProbabilityModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef ECHORADAR_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace EchoRadar {

struct OnnxProbabilityModel::Impl {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "EchoRadar"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
#endif
};

OnnxProbabilityModel::OnnxProbabilityModel(const std::filesystem::path& modelPath,
                                           size_t inputFrames,
                                           size_t inputBins,
                                           size_t inputChannels)
    : m_impl(std::make_unique<Impl>()), m_inputFrames(inputFrames), m_inputBins(inputBins),
      m_inputChannels(inputChannels) {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    try {
        m_impl->options.SetIntraOpNumThreads(1);
        m_impl->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        m_impl->session = std::make_unique<Ort::Session>(m_impl->environment,
                                                        modelPath.wstring().c_str(),
                                                        m_impl->options);
#else
        m_impl->session = std::make_unique<Ort::Session>(m_impl->environment,
                                                        modelPath.string().c_str(),
                                                        m_impl->options);
#endif
    } catch (const Ort::Exception& exception) {
        m_error = exception.what();
    }
#else
    (void)modelPath;
    m_error = "EchoRadar was built without ONNX Runtime; configure with -DECHORADAR_ENABLE_ONNX=ON and ONNXRUNTIME_ROOT";
#endif
}

OnnxProbabilityModel::~OnnxProbabilityModel() = default;

bool OnnxProbabilityModel::IsLoaded() const {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    return m_impl && m_impl->session != nullptr;
#else
    return false;
#endif
}

const std::string& OnnxProbabilityModel::LoadError() const {
    return m_error;
}

bool OnnxProbabilityModel::Predict(std::span<const float> logMel,
                                   std::array<float, kSoundClassCount>& probabilities,
                                   std::string* error) {
    probabilities = {};
    if (!IsLoaded()) {
        if (error) *error = m_error;
        return false;
    }
    if (logMel.size() != m_inputChannels * m_inputFrames * m_inputBins) {
        if (error) *error = "Unexpected ONNX input size";
        return false;
    }
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    try {
        const std::array<int64_t, 4> inputShape{1, static_cast<int64_t>(m_inputChannels),
                                                static_cast<int64_t>(m_inputFrames),
                                                static_cast<int64_t>(m_inputBins)};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(memory,
            const_cast<float*>(logMel.data()), logMel.size(), inputShape.data(), inputShape.size());
        const char* inputNames[] = {"logmel"};
        const char* outputNames[] = {"probabilities"};
        auto output = m_impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &input, 1,
                                           outputNames, 1);
        if (output.empty() || !output[0].IsTensor()) throw std::runtime_error("ONNX output is not a tensor");
        const auto info = output[0].GetTensorTypeAndShapeInfo();
        const size_t count = info.GetElementCount();
        if (count < kSoundClassCount || count % kSoundClassCount != 0) {
            throw std::runtime_error("ONNX output does not end in three class values");
        }
        const float* values = output[0].GetTensorData<float>() + count - kSoundClassCount;
        for (size_t i = 0; i < kSoundClassCount; ++i) probabilities[i] = std::clamp(values[i], 0.0f, 1.0f);
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
