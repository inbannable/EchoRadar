#include "SoundRecognizer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {
namespace {

StereoOnsetFeatureExtractor::Config FeatureConfig(const RecognitionModelPackage& package) {
    StereoOnsetFeatureExtractor::Config config;
    config.sampleRate = package.sampleRate;
    config.fftSize = package.fftSize;
    config.hopSize = package.hopSize;
    config.melBins = package.melBins;
    config.pcenSmoothing = package.pcenSmoothing;
    config.pcenAlpha = package.pcenAlpha;
    config.pcenDelta = package.pcenDelta;
    config.pcenRoot = package.pcenRoot;
    config.pcenEpsilon = package.pcenEpsilon;
    config.activitySmoothingFrames =
        static_cast<uint32_t>(std::lround(0.5 * package.sampleRate / package.hopSize));
    return config;
}

SoundClass RecognitionClass(size_t index) {
    return index == 0 ? SoundClass::Gunshot : SoundClass::Footstep;
}

} // namespace

SoundRecognizer::SoundRecognizer(std::shared_ptr<RecognitionProbabilityModel> model,
                           RecognitionModelPackage package,
                           EventCallback callback)
    : SoundRecognizer(std::move(model), std::move(package), std::move(callback), nullptr) {}

SoundRecognizer::SoundRecognizer(std::shared_ptr<RecognitionProbabilityModel> model,
                           RecognitionModelPackage package,
                           EventCallback callback,
                           std::shared_ptr<RecognitionRuntimeTuningStore> runtimeTuning)
    : m_model(std::move(model)),
      m_package(std::move(package)),
      m_callback(std::move(callback)),
      m_runtimeTuning(std::move(runtimeTuning)),
      m_features(FeatureConfig(m_package)) {
    if (!m_model || m_model->InputFrames() != m_package.contextFrames ||
        m_model->InputBins() != m_package.melBins ||
        m_model->InputChannels() != m_package.inputChannels) {
        throw std::invalid_argument("Recognition model shape is incompatible with its package");
    }
    m_input.resize(static_cast<size_t>(m_package.inputChannels) *
                   m_package.contextFrames * m_package.melBins);
}

std::vector<SoundEvent>
SoundRecognizer::PushInterleaved(const float* stereoSamples, size_t frameCount) {
    ApplyRuntimeTuning();
    std::vector<SoundEvent> events;
    m_features.PushInterleaved(stereoSamples, frameCount);
    m_stats.processedPcmFrames += frameCount;

    StereoOnsetFeatureFrame frame;
    while (m_features.PopFrame(frame)) {
        ++m_featureCount;
        ++m_stats.producedFeatureFrames;
        m_lastFeature = frame;
        m_haveLastFeature = true;
        m_context.push_back(frame);
        while (m_context.size() > m_package.contextFrames) m_context.pop_front();
        if ((m_featureCount - 1) % m_package.inferenceStrideFrames == 0) {
            RunInference(frame, events);
        }
    }
    Publish(events);
    return events;
}

void SoundRecognizer::RunInference(const StereoOnsetFeatureFrame& frame,
                                std::vector<SoundEvent>& events) {
    std::fill(m_input.begin(), m_input.end(), 0.0f);
    const size_t planeSize = static_cast<size_t>(m_package.contextFrames) * m_package.melBins;
    std::fill(m_input.begin() + static_cast<std::ptrdiff_t>(planeSize),
              m_input.begin() + static_cast<std::ptrdiff_t>(2 * planeSize), -100.0f);
    const size_t startFrame = m_package.contextFrames - m_context.size();
    size_t contextIndex = 0;
    for (const auto& contextFrame : m_context) {
        const size_t outputFrame = startFrame + contextIndex++;
        for (size_t channel = 0; channel < m_package.inputChannels; ++channel) {
            const size_t destination =
                (channel * m_package.contextFrames + outputFrame) * m_package.melBins;
            const size_t source = channel * m_package.melBins;
            std::copy_n(contextFrame.values.begin() + static_cast<std::ptrdiff_t>(source),
                        m_package.melBins,
                        m_input.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }

    RecognitionModelOutput output;
    const auto start = std::chrono::steady_clock::now();
    const bool predicted = m_model->Predict(m_input, output, &m_lastError);
    const auto finish = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>(finish - start).count();
    ++m_stats.inferenceCount;
    m_stats.lastInferenceMs = elapsed;
    m_stats.maxInferenceMs = std::max(m_stats.maxInferenceMs, elapsed);
    m_inferenceTimes[m_inferenceTimeIndex] = elapsed;
    m_inferenceTimeIndex = (m_inferenceTimeIndex + 1) % m_inferenceTimes.size();
    m_inferenceTimeCount = std::min(m_inferenceTimeCount + 1, m_inferenceTimes.size());
    auto ordered = m_inferenceTimes;
    std::sort(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(m_inferenceTimeCount));
    const size_t percentile = m_inferenceTimeCount == 0 ? 0 :
        std::min(m_inferenceTimeCount - 1,
                 static_cast<size_t>(m_inferenceTimeCount * 0.95));
    m_stats.p95InferenceMs = ordered[percentile];
    m_lastInferenceFeatureCount = m_featureCount;
    if (!predicted) return;

    m_lastOutput = output;
    m_trace.push_back(TracePoint{frame.endSample, output, frame.sceneActivity});
    m_latestTraceSample = frame.endSample;
    ++m_totalTraceCount;
    ProcessReadyPeaks(events, false);
}

const SoundRecognizer::TracePoint& SoundRecognizer::TraceAt(uint64_t index) const {
    if (index < m_traceStartIndex || index >= m_traceStartIndex + m_trace.size()) {
        throw std::out_of_range("Recognition probability trace was pruned too early");
    }
    return m_trace[static_cast<size_t>(index - m_traceStartIndex)];
}

void SoundRecognizer::EvaluatePeak(uint64_t index, uint64_t rightIndex,
                                std::vector<SoundEvent>& events) {
    const TracePoint& point = TraceAt(index);
    const uint64_t lookahead = m_package.peakLookaheadFrames;
    const uint64_t leftIndex = index > lookahead ? index - lookahead : 0;
    const uint64_t cappedRight = std::min(rightIndex, m_totalTraceCount - 1);
    for (size_t classIndex = 0; classIndex < kSoundClassCount; ++classIndex) {
        const float probability = point.output.onsetProbabilities[classIndex];
        const bool quiet = point.activity < m_package.sceneActivityCutoff;
        const float threshold = quiet ? m_package.quietThresholds[classIndex]
                                      : m_package.busyThresholds[classIndex];
        float localMaximum = 0.0f;
        float earlierMaximum = -1.0f;
        for (uint64_t cursor = leftIndex; cursor <= cappedRight; ++cursor) {
            const float value = TraceAt(cursor).output.onsetProbabilities[classIndex];
            localMaximum = std::max(localMaximum, value);
            if (cursor < index) earlierMaximum = std::max(earlierMaximum, value);
        }
        if (probability < threshold || probability < localMaximum ||
            (index > leftIndex && probability <= earlierMaximum)) {
            continue;
        }
        ConsiderCandidate(classIndex, point.sample, TraceAt(cappedRight).sample, point, events);
    }

    const uint64_t latestSample = m_latestTraceSample;
    for (size_t classIndex = 0; classIndex < kSoundClassCount; ++classIndex) {
        if (m_pending[classIndex].active &&
            point.sample - m_pending[classIndex].sample >=
                m_package.minimumSpacingSamples[classIndex]) {
            EmitPending(classIndex, latestSample, events);
        }
    }
}

void SoundRecognizer::ConsiderCandidate(size_t classIndex, uint64_t sample,
                                     uint64_t detectedSample, const TracePoint& point,
                                     std::vector<SoundEvent>& events) {
    PendingCandidate candidate;
    candidate.active = true;
    candidate.sample = sample;
    candidate.detectedSample = detectedSample;
    candidate.confidence = point.output.onsetProbabilities[classIndex];
    const auto& sourceValues = point.output.sourceProbabilities[classIndex];
    const size_t sourceIndex = static_cast<size_t>(
        std::distance(sourceValues.begin(), std::max_element(sourceValues.begin(), sourceValues.end())));
    candidate.source = static_cast<SoundSourceHint>(sourceIndex);
    candidate.sourceConfidence = sourceValues[sourceIndex];
    if (sourceValues[0] >= m_package.selfSuppressionThreshold) {
        candidate.source = SoundSourceHint::Self;
        candidate.sourceConfidence = sourceValues[0];
        candidate.suppressed = true;
    }
    candidate.scene = point.activity < m_package.sceneActivityCutoff
        ? SceneState::Quiet : SceneState::Busy;

    PendingCandidate& pending = m_pending[classIndex];
    if (pending.active) {
        if (sample - pending.sample < m_package.minimumSpacingSamples[classIndex]) {
            if (candidate.confidence > pending.confidence) pending = candidate;
            return;
        }
        EmitPending(classIndex, detectedSample, events);
    }
    pending = candidate;
}

void SoundRecognizer::EmitPending(size_t classIndex, uint64_t deliveredSample,
                               std::vector<SoundEvent>& events) {
    PendingCandidate& pending = m_pending[classIndex];
    if (!pending.active) return;
    SoundEvent event;
    event.soundClass = RecognitionClass(classIndex);
    event.onsetSample = pending.sample > m_package.onsetOffsetSamples[classIndex]
        ? pending.sample - m_package.onsetOffsetSamples[classIndex] : 0;
    event.endSample = event.onsetSample + m_package.pulseSamples;
    event.detectedSample = pending.detectedSample;
    event.deliveredSample = deliveredSample;
    event.confidence = pending.confidence;
    event.sourceHint = pending.source;
    event.sourceConfidence = pending.sourceConfidence;
    event.sceneState = pending.scene;
    event.suppressed = pending.suppressed;
    event.streamGeneration = m_streamGeneration;
    event.modelVersion = m_package.modelVersion;
    if (event.suppressed) ++m_stats.suppressedEventCount;
    events.push_back(std::move(event));
    pending = {};
}

void SoundRecognizer::PruneTrace() {
    const uint64_t lookahead = m_package.peakLookaheadFrames;
    const uint64_t needed = m_nextPeakIndex > lookahead ? m_nextPeakIndex - lookahead : 0;
    while (!m_trace.empty() && m_traceStartIndex < needed) {
        m_trace.pop_front();
        ++m_traceStartIndex;
    }
}

void SoundRecognizer::ProcessReadyPeaks(std::vector<SoundEvent>& events, bool flush) {
    const uint64_t lookahead = m_package.peakLookaheadFrames;
    while (m_nextPeakIndex < m_totalTraceCount &&
           (flush || m_nextPeakIndex + lookahead < m_totalTraceCount)) {
        const uint64_t right = flush
            ? std::min(m_totalTraceCount - 1, m_nextPeakIndex + lookahead)
            : m_nextPeakIndex + lookahead;
        EvaluatePeak(m_nextPeakIndex, right, events);
        ++m_nextPeakIndex;
        PruneTrace();
    }
    if (flush && m_totalTraceCount != 0) {
        for (size_t classIndex = 0; classIndex < kSoundClassCount; ++classIndex) {
            EmitPending(classIndex, m_latestTraceSample, events);
        }
    }
}

std::vector<SoundEvent> SoundRecognizer::Flush() {
    std::vector<SoundEvent> events;
    if (m_haveLastFeature && m_lastInferenceFeatureCount != m_featureCount) {
        RunInference(m_lastFeature, events);
    }
    ProcessReadyPeaks(events, true);
    Publish(events);
    return events;
}

void SoundRecognizer::Publish(const std::vector<SoundEvent>& events) {
    if (!m_callback) return;
    for (const auto& event : events) {
        if (!event.suppressed) m_callback(event);
    }
}

void SoundRecognizer::Reset() {
    m_features.Reset();
    m_context.clear();
    m_input.assign(m_input.size(), 0.0f);
    m_featureCount = 0;
    m_lastInferenceFeatureCount = 0;
    m_haveLastFeature = false;
    m_trace.clear();
    m_traceStartIndex = 0;
    m_totalTraceCount = 0;
    m_nextPeakIndex = 0;
    m_latestTraceSample = 0;
    m_pending = {};
    m_inferenceTimes = {};
    m_inferenceTimeCount = 0;
    m_inferenceTimeIndex = 0;
    m_lastOutput = {};
    m_stats = {};
    m_lastError.clear();
}

void SoundRecognizer::OnAudio(const AudioBlockView& block) {
    if (block.sampleRate != 48000 || block.channels != 2 ||
        block.interleaved.size() < block.frameCount * 2) {
        m_lastError = "SoundRecognizer requires 48 kHz interleaved stereo PCM";
        return;
    }
    PushInterleaved(block.interleaved.data(), block.frameCount);
}

void SoundRecognizer::OnStreamReset(uint64_t streamGeneration) {
    Reset();
    m_streamGeneration = streamGeneration;
}

void SoundRecognizer::ApplyRuntimeTuning() {
    if (!m_runtimeTuning) return;
    const RecognitionRuntimeTuning tuning = m_runtimeTuning->Snapshot();
    if (m_haveAppliedTuning && tuning == m_appliedTuning) return;
    tuning.ApplyTo(m_package);
    m_appliedTuning = tuning;
    m_haveAppliedTuning = true;
}

} // namespace EchoRadar
