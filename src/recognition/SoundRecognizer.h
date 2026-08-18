#pragma once

#include "StereoOnsetFeatureExtractor.h"
#include "RecognitionModelPackage.h"
#include "RecognitionProbabilityModel.h"
#include "RecognitionRuntimeConfig.h"

#include <audio/AudioStreamConsumer.h>

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EchoRadar {

class SoundRecognizer final : public IRealtimeAudioConsumer {
public:
    struct RuntimeStats {
        uint64_t processedPcmFrames{0};
        uint64_t producedFeatureFrames{0};
        uint64_t inferenceCount{0};
        uint64_t suppressedEventCount{0};
        double lastInferenceMs{0.0};
        double p95InferenceMs{0.0};
        double maxInferenceMs{0.0};
    };

    using EventCallback = std::function<void(const SoundEvent&)>;

    SoundRecognizer(std::shared_ptr<RecognitionProbabilityModel> model,
                 RecognitionModelPackage package,
                 EventCallback callback = {});
    SoundRecognizer(std::shared_ptr<RecognitionProbabilityModel> model,
                 RecognitionModelPackage package,
                 EventCallback callback,
                 std::shared_ptr<RecognitionRuntimeTuningStore> runtimeTuning);

    std::vector<SoundEvent> PushInterleaved(const float* stereoSamples, size_t frameCount);
    std::vector<SoundEvent> Flush();
    void Reset();

    void OnAudio(const AudioBlockView& block) override;
    void OnStreamReset(uint64_t streamGeneration) override;

    const RecognitionModelOutput& LastOutput() const { return m_lastOutput; }
    float LastSceneActivity() const {
        return m_haveLastFeature ? m_lastFeature.sceneActivity : 0.0f;
    }
    const RuntimeStats& Stats() const { return m_stats; }
    const std::string& LastError() const { return m_lastError; }
    uint64_t StreamGeneration() const { return m_streamGeneration; }

private:
    struct TracePoint {
        uint64_t sample{0};
        RecognitionModelOutput output;
        float activity{0.0f};
    };

    struct PendingCandidate {
        bool active{false};
        uint64_t sample{0};
        uint64_t detectedSample{0};
        float confidence{0.0f};
        SoundSourceHint source{SoundSourceHint::Unknown};
        float sourceConfidence{0.0f};
        SceneState scene{SceneState::Quiet};
        bool suppressed{false};
    };

    std::shared_ptr<RecognitionProbabilityModel> m_model;
    RecognitionModelPackage m_package;
    EventCallback m_callback;
    std::shared_ptr<RecognitionRuntimeTuningStore> m_runtimeTuning;
    RecognitionRuntimeTuning m_appliedTuning{};
    bool m_haveAppliedTuning{false};
    StereoOnsetFeatureExtractor m_features;
    std::deque<StereoOnsetFeatureFrame> m_context;
    std::vector<float> m_input;
    uint64_t m_featureCount{0};
    uint64_t m_lastInferenceFeatureCount{0};
    StereoOnsetFeatureFrame m_lastFeature;
    bool m_haveLastFeature{false};

    std::deque<TracePoint> m_trace;
    uint64_t m_traceStartIndex{0};
    uint64_t m_totalTraceCount{0};
    uint64_t m_nextPeakIndex{0};
    uint64_t m_latestTraceSample{0};
    std::array<PendingCandidate, kSoundClassCount> m_pending{};

    std::array<double, 256> m_inferenceTimes{};
    size_t m_inferenceTimeCount{0};
    size_t m_inferenceTimeIndex{0};
    RecognitionModelOutput m_lastOutput;
    RuntimeStats m_stats;
    std::string m_lastError;
    uint64_t m_streamGeneration{0};

    void RunInference(const StereoOnsetFeatureFrame& frame,
                      std::vector<SoundEvent>& events);
    void ProcessReadyPeaks(std::vector<SoundEvent>& events, bool flush);
    void EvaluatePeak(uint64_t index, uint64_t rightIndex,
                      std::vector<SoundEvent>& events);
    void ConsiderCandidate(size_t classIndex, uint64_t sample, uint64_t detectedSample,
                           const TracePoint& point, std::vector<SoundEvent>& events);
    void EmitPending(size_t classIndex, uint64_t deliveredSample,
                     std::vector<SoundEvent>& events);
    const TracePoint& TraceAt(uint64_t index) const;
    void PruneTrace();
    void Publish(const std::vector<SoundEvent>& events);
    void ApplyRuntimeTuning();
};

} // namespace EchoRadar
