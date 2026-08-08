#include "EventPostProcessor.h"

#include <algorithm>
#include <stdexcept>

namespace EchoRadar {

EventPostProcessor::EventPostProcessor(Config config) : m_config(std::move(config)) {
    for (size_t i = 0; i < kSoundClassCount; ++i) {
        if (m_config.onThresholds[i] <= 0.0f || m_config.onThresholds[i] > 1.0f ||
            m_config.offThresholds[i] < 0.0f || m_config.offThresholds[i] >= m_config.onThresholds[i] ||
            m_config.rearmThresholds[i] < 0.0f ||
            m_config.rearmThresholds[i] >= m_config.onThresholds[i] ||
            m_config.minOnFrames[i] == 0 || m_config.minOffFrames[i] == 0 ||
            (m_config.mode == Mode::OnsetPulse && m_config.pulseSamples == 0)) {
            throw std::invalid_argument("Invalid sound event post-processing configuration");
        }
    }
}

std::vector<SoundEvent> EventPostProcessor::Process(const SoundProbabilities& probabilities) {
    std::vector<SoundEvent> events;
    for (size_t index = 0; index < kSoundClassCount; ++index) {
        State& state = m_states[index];
        const float probability = std::clamp(probabilities.values[index], 0.0f, 1.0f);
        if (m_config.mode == Mode::OnsetPulse) {
            if (state.active && probabilities.sample >= state.activeUntil) state.active = false;
            if (!state.armed) {
                if (probability < m_config.rearmThresholds[index]) state.armed = true;
                continue;
            }
            if (probabilities.sample < state.refractoryUntil ||
                probability < m_config.onThresholds[index]) {
                continue;
            }
            const uint64_t onset = probabilities.sample > m_config.onsetOffsetSamples[index]
                ? probabilities.sample - m_config.onsetOffsetSamples[index] : 0;
            SoundEvent event;
            event.soundClass = kSoundClasses[index];
            event.onsetSample = onset;
            event.endSample = onset + m_config.pulseSamples;
            event.confidence = probability;
            event.modelVersion = m_config.modelVersion;
            event.detectedSample = probabilities.sample;
            events.push_back(std::move(event));
            state.armed = false;
            state.active = true;
            state.activeUntil = onset + m_config.pulseSamples;
            state.refractoryUntil = probabilities.sample + m_config.refractorySamples[index];
            continue;
        }
        if (!state.active) {
            if (probabilities.sample < state.refractoryUntil || probability < m_config.onThresholds[index]) {
                state.aboveCount = 0;
                continue;
            }
            if (state.aboveCount == 0) state.candidateOnset = probabilities.sample;
            ++state.aboveCount;
            state.peak = std::max(state.peak, probability);
            if (state.aboveCount >= m_config.minOnFrames[index]) {
                state.active = true;
                state.onset = state.candidateOnset;
                state.belowCount = 0;
            }
            continue;
        }

        state.peak = std::max(state.peak, probability);
        if (probability < m_config.offThresholds[index]) ++state.belowCount;
        else state.belowCount = 0;
        if (state.belowCount < m_config.minOffFrames[index]) continue;

        SoundEvent event;
        event.soundClass = kSoundClasses[index];
        event.onsetSample = state.onset;
        event.endSample = probabilities.sample;
        event.confidence = state.peak;
        event.modelVersion = m_config.modelVersion;
        event.detectedSample = probabilities.sample;
        events.push_back(std::move(event));
        state = {};
        state.refractoryUntil = probabilities.sample + m_config.refractorySamples[index];
    }
    return events;
}

std::vector<SoundEvent> EventPostProcessor::Flush(uint64_t endSample) {
    std::vector<SoundEvent> events;
    if (m_config.mode == Mode::OnsetPulse) {
        Reset();
        return events;
    }
    for (size_t index = 0; index < kSoundClassCount; ++index) {
        State& state = m_states[index];
        if (!state.active) continue;
        events.push_back(SoundEvent{kSoundClasses[index], state.onset, endSample,
                                    state.peak, m_config.modelVersion, endSample});
    }
    Reset();
    return events;
}

void EventPostProcessor::Reset() {
    m_states = {};
}

bool EventPostProcessor::IsActive(SoundClass soundClass) const {
    return m_states[SoundClassIndex(soundClass)].active;
}

std::array<bool, kSoundClassCount> EventPostProcessor::ActiveClasses() const {
    std::array<bool, kSoundClassCount> active{};
    for (size_t index = 0; index < kSoundClassCount; ++index) {
        active[index] = m_states[index].active;
    }
    return active;
}

bool EventPostProcessor::IsAmbient() const {
    return std::none_of(m_states.begin(), m_states.end(),
                        [](const State& state) { return state.active; });
}

} // namespace EchoRadar
