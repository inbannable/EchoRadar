#pragma once

#include "V4ModelPackage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace EchoRadar {

/// Policy values that are safe to change while the V4 recognizer is running.
///
/// The feature extractor and model tensor shape remain locked to the exported
/// package contract. These values affect only event calibration and delivery.
struct V4RuntimeTuning {
    std::array<float, 2> quietThresholds{};
    std::array<float, 2> busyThresholds{};
    std::array<uint32_t, 2> minimumSpacingMs{};
    std::array<uint32_t, 2> onsetOffsetSamples{};
    float sceneActivityCutoff{0.5f};
    float selfSuppressionThreshold{0.95f};
    uint32_t pulseMs{50};

    friend bool operator==(const V4RuntimeTuning&, const V4RuntimeTuning&) = default;

    static V4RuntimeTuning FromPackage(const V4ModelPackage& package) {
        V4RuntimeTuning tuning;
        tuning.quietThresholds = package.quietThresholds;
        tuning.busyThresholds = package.busyThresholds;
        tuning.sceneActivityCutoff = package.sceneActivityCutoff;
        tuning.selfSuppressionThreshold = package.selfSuppressionThreshold;

        const uint32_t sampleRate = package.sampleRate == 0 ? 48000 : package.sampleRate;
        for (size_t index = 0; index < tuning.minimumSpacingMs.size(); ++index) {
            tuning.minimumSpacingMs[index] = ToMilliseconds(
                package.minimumSpacingSamples[index], sampleRate, /*minimum=*/1);
            tuning.onsetOffsetSamples[index] = package.onsetOffsetSamples[index];
        }
        tuning.pulseMs = ToMilliseconds(package.pulseSamples, sampleRate, /*minimum=*/1);
        return Clamp(tuning);
    }

    /// Apply UI values to the recognizer's sample-based package policy.
    void ApplyTo(V4ModelPackage& package) const {
        const V4RuntimeTuning safe = Clamp(*this);
        package.quietThresholds = safe.quietThresholds;
        package.busyThresholds = safe.busyThresholds;
        package.sceneActivityCutoff = safe.sceneActivityCutoff;
        package.selfSuppressionThreshold = safe.selfSuppressionThreshold;

        const uint32_t sampleRate = package.sampleRate == 0 ? 48000 : package.sampleRate;
        for (size_t index = 0; index < safe.minimumSpacingMs.size(); ++index) {
            package.minimumSpacingSamples[index] =
                std::max<uint64_t>(1, static_cast<uint64_t>(safe.minimumSpacingMs[index]) *
                                      sampleRate / 1000u);
            package.onsetOffsetSamples[index] = safe.onsetOffsetSamples[index];
        }
        package.pulseSamples = std::max<uint64_t>(
            1, static_cast<uint64_t>(safe.pulseMs) * sampleRate / 1000u);
    }

    static V4RuntimeTuning Clamp(V4RuntimeTuning tuning) {
        for (size_t index = 0; index < tuning.quietThresholds.size(); ++index) {
            tuning.quietThresholds[index] =
                std::clamp(FiniteOr(tuning.quietThresholds[index], 0.95f), 0.01f, 1.0f);
            tuning.busyThresholds[index] =
                std::clamp(FiniteOr(tuning.busyThresholds[index], 0.95f), 0.01f, 1.0f);
            tuning.minimumSpacingMs[index] = std::clamp(tuning.minimumSpacingMs[index], 1u, 5000u);
            tuning.onsetOffsetSamples[index] = std::min(tuning.onsetOffsetSamples[index], 24000u);
        }
        tuning.sceneActivityCutoff =
            std::clamp(FiniteOr(tuning.sceneActivityCutoff, 0.5f), 0.01f, 0.99f);
        tuning.selfSuppressionThreshold =
            std::clamp(FiniteOr(tuning.selfSuppressionThreshold, 0.95f), 0.01f, 1.0f);
        tuning.pulseMs = std::clamp(tuning.pulseMs, 1u, 1000u);
        return tuning;
    }

private:
    static float FiniteOr(float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    }

    static uint32_t ToMilliseconds(uint64_t samples, uint32_t sampleRate,
                                   uint32_t minimum) {
        const uint64_t rounded = (samples * 1000u + sampleRate / 2u) / sampleRate;
        return std::max<uint32_t>(minimum, static_cast<uint32_t>(rounded));
    }
};

/// Small synchronized hand-off between the ImGui thread and the DSP thread.
class V4RuntimeTuningStore {
public:
    explicit V4RuntimeTuningStore(V4RuntimeTuning initial)
        : m_default(V4RuntimeTuning::Clamp(initial)),
          m_value(m_default) {}

    V4RuntimeTuning Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_value;
    }

    void Update(const V4RuntimeTuning& tuning) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_value = V4RuntimeTuning::Clamp(tuning);
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_value = m_default;
    }

private:
    V4RuntimeTuning m_default;
    V4RuntimeTuning m_value;
    mutable std::mutex m_mutex;
};

} // namespace EchoRadar
