#pragma once

#include "StereoOnsetFeatureExtractor.h"
#include "V4ModelPackage.h"
#include "V4ProbabilityModel.h"
#include "V4RuntimeConfig.h"

#include <audio/AudioStreamConsumer.h>

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EchoRadar {

class V4Recognizer final : public IRealtimeAudioConsumer {
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

    using EventCallback = std::function<void(const V4SoundEvent&)>;

    V4Recognizer(std::shared_ptr<V4ProbabilityModel> model,
                 V4ModelPackage package,
                 EventCallback callback = {});
    V4Recognizer(std::shared_ptr<V4ProbabilityModel> model,
                 V4ModelPackage package,
                 EventCallback callback,
                 std::shared_ptr<V4RuntimeTuningStore> runtimeTuning);

    std::vector<V4SoundEvent> PushInterleaved(const float* stereoSamples, size_t frameCount);
    std::vector<V4SoundEvent> Flush();
    void Reset();

    void OnAudio(const AudioBlockView& block) override;
    void OnStreamReset(uint64_t streamGeneration) override;

    const V4ModelOutput& LastOutput() const { return m_lastOutput; }
    const RuntimeStats& Stats() const { return m_stats; }
    const std::string& LastError() const { return m_lastError; }
    uint64_t StreamGeneration() const { return m_streamGeneration; }

private:
    struct TracePoint {
        uint64_t sample{0};
        V4ModelOutput output;
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

    std::shared_ptr<V4ProbabilityModel> m_model;
    V4ModelPackage m_package;
    EventCallback m_callback;
    std::shared_ptr<V4RuntimeTuningStore> m_runtimeTuning;
    V4RuntimeTuning m_appliedTuning{};
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
    std::array<PendingCandidate, kV4SoundClassCount> m_pending{};

    std::array<double, 256> m_inferenceTimes{};
    size_t m_inferenceTimeCount{0};
    size_t m_inferenceTimeIndex{0};
    V4ModelOutput m_lastOutput;
    RuntimeStats m_stats;
    std::string m_lastError;
    uint64_t m_streamGeneration{0};

    void RunInference(const StereoOnsetFeatureFrame& frame,
                      std::vector<V4SoundEvent>& events);
    void ProcessReadyPeaks(std::vector<V4SoundEvent>& events, bool flush);
    void EvaluatePeak(uint64_t index, uint64_t rightIndex,
                      std::vector<V4SoundEvent>& events);
    void ConsiderCandidate(size_t classIndex, uint64_t sample, uint64_t detectedSample,
                           const TracePoint& point, std::vector<V4SoundEvent>& events);
    void EmitPending(size_t classIndex, uint64_t deliveredSample,
                     std::vector<V4SoundEvent>& events);
    const TracePoint& TraceAt(uint64_t index) const;
    void PruneTrace();
    void Publish(const std::vector<V4SoundEvent>& events);
    void ApplyRuntimeTuning();
};

} // namespace EchoRadar
