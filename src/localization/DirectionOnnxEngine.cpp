#include "DirectionOnnxEngine.h"

#include <recognition/StereoOnsetFeatureExtractor.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef ECHORADAR_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace EchoRadar {
namespace {

struct Candidate {
    size_t classIndex{0};
    size_t trackIndex{0};
    std::array<float, 3> vector{};
    float confidence{0.0f};
};

float Norm(const std::array<float, 3>& value) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

float AngularDistance(const std::array<float, 3>& left,
                      const std::array<float, 3>& right) {
    const float denominator = Norm(left) * Norm(right);
    if (denominator <= 1.0e-9f) return 180.0f;
    const float cosine = std::clamp(
        (left[0] * right[0] + left[1] * right[1] + left[2] * right[2]) /
            denominator,
        -1.0f, 1.0f);
    return std::acos(cosine) * 180.0f / 3.14159265358979323846f;
}

DirectionSourceEstimate Estimate(const DirectionModelPackage& package,
                                 const Candidate& candidate) {
    const float horizontal = std::hypot(candidate.vector[0], candidate.vector[2]);
    float azimuth = std::atan2(candidate.vector[0], candidate.vector[2]) *
        180.0f / 3.14159265358979323846f;
    if (azimuth < 0.0f) azimuth += 360.0f;
    const float elevation = std::clamp(
        std::atan2(candidate.vector[1], horizontal) *
            180.0f / 3.14159265358979323846f,
        package.elevationMinDegrees, package.elevationMaxDegrees);
    return {
        azimuth,
        elevation,
        std::clamp(candidate.confidence, 0.0f, 1.0f),
        package.UncertaintyDegrees(candidate.confidence),
    };
}

} // namespace

struct DirectionOnnxEngine::Impl {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "EchoRadarDirection"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
#endif
};

DirectionOnnxEngine::DirectionOnnxEngine(DirectionModelPackage package)
    : m_package(std::move(package)), m_impl(std::make_unique<Impl>()) {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    try {
        m_impl->options.SetIntraOpNumThreads(1);
        m_impl->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        m_impl->session = std::make_unique<Ort::Session>(
            m_impl->environment, m_package.modelPath.wstring().c_str(), m_impl->options);
#else
        m_impl->session = std::make_unique<Ort::Session>(
            m_impl->environment, m_package.modelPath.string().c_str(), m_impl->options);
#endif
    } catch (const Ort::Exception& exception) {
        m_error = exception.what();
    }
#else
    m_error = "EchoRadar was built without ONNX Runtime; configure with "
              "-DECHORADAR_ENABLE_ONNX=ON and ONNXRUNTIME_ROOT";
#endif
}

DirectionOnnxEngine::~DirectionOnnxEngine() = default;

bool DirectionOnnxEngine::IsLoaded() const {
#ifdef ECHORADAR_HAS_ONNXRUNTIME
    return m_impl && m_impl->session != nullptr;
#else
    return false;
#endif
}

bool DirectionOnnxEngine::ExtractFeatures(
    std::span<const float> interleavedStereo,
    std::vector<float>& features,
    std::string* error) const {
    features.clear();
    const size_t expectedValues = static_cast<size_t>(m_package.contextSamples) * 2u;
    if (interleavedStereo.size() != expectedValues) {
        if (error) *error = "Direction scene must contain exactly 12,304 stereo frames";
        return false;
    }
    try {
        StereoOnsetFeatureExtractor extractor({
            m_package.sampleRate,
            m_package.fftSize,
            m_package.hopSize,
            m_package.melBins,
            50.0f,
            18000.0f,
            m_package.pcenSmoothing,
            m_package.pcenAlpha,
            m_package.pcenDelta,
            m_package.pcenRoot,
            m_package.pcenEpsilon,
            100,
        });
        extractor.PushInterleaved(interleavedStereo.data(), m_package.contextSamples);
        features.assign(
            static_cast<size_t>(m_package.inputChannels) * m_package.contextFrames *
                m_package.melBins,
            0.0f);
        StereoOnsetFeatureFrame frame;
        uint32_t frameIndex = 0;
        while (extractor.PopFrame(frame)) {
            if (frameIndex >= m_package.contextFrames) {
                if (error) *error = "Direction feature extractor produced too many frames";
                features.clear();
                return false;
            }
            for (uint32_t channel = 0; channel < m_package.inputChannels; ++channel) {
                for (uint32_t mel = 0; mel < m_package.melBins; ++mel) {
                    const size_t destination =
                        (static_cast<size_t>(channel) * m_package.contextFrames + frameIndex) *
                            m_package.melBins + mel;
                    const size_t source = static_cast<size_t>(channel) * m_package.melBins + mel;
                    features[destination] = frame.values[source];
                }
            }
            ++frameIndex;
        }
        if (frameIndex != m_package.contextFrames ||
            !std::all_of(features.begin(), features.end(), [](float value) {
                return std::isfinite(value);
            })) {
            if (error) *error = "Direction feature extractor did not produce 48 finite frames";
            features.clear();
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        features.clear();
        return false;
    }
}

DirectionSceneResult DirectionOnnxEngine::Predict(
    uint64_t sceneId,
    uint64_t streamGeneration,
    uint64_t anchorEventSample,
    uint64_t sceneStartSample,
    std::span<const float> interleavedStereo,
    uint8_t enabledClassMask,
    std::string* error) {
    DirectionSceneResult result;
    result.sceneId = sceneId;
    result.streamGeneration = streamGeneration;
    result.anchorEventSample = anchorEventSample;
    result.sceneStartSample = sceneStartSample;
    result.sceneEndSample = sceneStartSample + m_package.contextSamples;
    result.enabledClassMask = enabledClassMask;
    result.featureFrames = m_package.contextFrames;
    result.inputChannels = m_package.inputChannels;
    result.melBins = m_package.melBins;
    result.sampleRate = m_package.sampleRate;
    result.modelVersion = m_package.modelVersion;
    result.preprocessingVersion = m_package.preprocessingVersion;
    if (enabledClassMask == 0) {
        result.status = DirectionStatus::Disabled;
        if (error) error->clear();
        return result;
    }
    if (!IsLoaded()) {
        result.status = DirectionStatus::ModelUnavailable;
        if (error) *error = m_error;
        return result;
    }
    std::vector<float> features;
    if (!ExtractFeatures(interleavedStereo, features, error)) {
        result.status = DirectionStatus::AudioUnavailable;
        return result;
    }

#ifdef ECHORADAR_HAS_ONNXRUNTIME
    const auto inferenceStart = std::chrono::steady_clock::now();
    try {
        const std::array<int64_t, 4> inputShape{
            1,
            static_cast<int64_t>(m_package.inputChannels),
            static_cast<int64_t>(m_package.contextFrames),
            static_cast<int64_t>(m_package.melBins),
        };
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, features.data(), features.size(), inputShape.data(), inputShape.size());
        const char* inputNames[]{"features"};
        const char* outputNames[]{"multi_accdoa"};
        auto outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 1);
        if (outputs.size() != 1 || !outputs[0].IsTensor() ||
            outputs[0].GetTensorTypeAndShapeInfo().GetShape() !=
                std::vector<int64_t>{1, 2, 3, 3}) {
            throw std::runtime_error("Direction ONNX output shape must be [1,2,3,3]");
        }
        RawOutput raw;
        const float* values = outputs[0].GetTensorData<float>();
        std::copy_n(values, raw.size(), raw.begin());
        if (!std::all_of(raw.begin(), raw.end(), [](float value) {
                return std::isfinite(value);
            })) {
            throw std::runtime_error("Direction ONNX output contains a non-finite value");
        }
        result.inferenceMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - inferenceStart).count();
        result = PostProcess(m_package, raw, enabledClassMask, std::move(result));
        if (error) error->clear();
        return result;
    } catch (const std::exception& exception) {
        result.inferenceMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - inferenceStart).count();
        result.status = DirectionStatus::InferenceFailed;
        if (error) *error = exception.what();
        return result;
    }
#else
    result.status = DirectionStatus::ModelUnavailable;
    if (error) *error = m_error;
    return result;
#endif
}

DirectionSceneResult DirectionOnnxEngine::PostProcess(
    const DirectionModelPackage& package,
    const RawOutput& output,
    uint8_t enabledClassMask,
    DirectionSceneResult result) {
    result.enabledClassMask = enabledClassMask;
    result.sourceCount = 0;
    result.sources = {};
    if (enabledClassMask == 0) {
        result.status = DirectionStatus::Disabled;
        return result;
    }
    std::vector<Candidate> candidates;
    for (size_t classIndex = 0; classIndex < DirectionModelPackage::kClassCount; ++classIndex) {
        const uint8_t classBit = static_cast<uint8_t>(1u << classIndex);
        if ((enabledClassMask & classBit) == 0) continue;
        for (size_t trackIndex = 0; trackIndex < DirectionModelPackage::kTrackCount; ++trackIndex) {
            const size_t offset = (classIndex * DirectionModelPackage::kTrackCount + trackIndex) * 3u;
            Candidate candidate{
                classIndex,
                trackIndex,
                {output[offset], output[offset + 1], output[offset + 2]},
                0.0f,
            };
            const float vectorNorm = Norm(candidate.vector);
            if (!std::isfinite(vectorNorm)) {
                continue;
            }
            candidate.confidence = std::clamp(vectorNorm, 0.0f, 1.0f);
            if (candidate.confidence < package.activityThresholds[classIndex]) continue;
            const auto duplicate = std::find_if(candidates.begin(), candidates.end(),
                [&](const Candidate& old) {
                    return old.classIndex == candidate.classIndex &&
                        AngularDistance(old.vector, candidate.vector) < package.duplicateMergeDegrees;
                });
            if (duplicate == candidates.end()) {
                candidates.push_back(candidate);
            } else {
                const float oldWeight = duplicate->confidence;
                for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
                    duplicate->vector[coordinate] =
                        duplicate->vector[coordinate] * oldWeight +
                        candidate.vector[coordinate] * candidate.confidence;
                }
                duplicate->confidence = std::max(oldWeight, candidate.confidence);
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.confidence > right.confidence;
        });
    result.sourceCount = static_cast<uint32_t>(std::min<size_t>(
        package.maximumSources, candidates.size()));
    for (uint32_t index = 0; index < result.sourceCount; ++index) {
        result.sources[index] = Estimate(package, candidates[index]);
    }
    result.status = result.sourceCount > 0
        ? DirectionStatus::Estimated : DirectionStatus::LowConfidence;
    return result;
}

} // namespace EchoRadar
