#pragma once

#include "SoundRecognitionTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace EchoRadar {

class EventPostProcessor {
public:
    enum class Mode {
        Segment,
        OnsetPulse,
    };

    struct Config {
        Mode mode{Mode::Segment};
        std::array<float, kSoundClassCount> onThresholds{0.50f, 0.50f, 0.50f};
        std::array<float, kSoundClassCount> offThresholds{0.35f, 0.35f, 0.35f};
        std::array<float, kSoundClassCount> rearmThresholds{0.25f, 0.25f, 0.25f};
        std::array<uint32_t, kSoundClassCount> minOnFrames{1, 1, 1};
        std::array<uint32_t, kSoundClassCount> minOffFrames{2, 2, 2};
        std::array<uint64_t, kSoundClassCount> refractorySamples{2400, 2400, 4800};
        std::array<uint64_t, kSoundClassCount> onsetOffsetSamples{};
        uint64_t pulseSamples{2400};
        std::string modelVersion;
    };

    EventPostProcessor();
    explicit EventPostProcessor(Config config);

    std::vector<SoundEvent> Process(const SoundProbabilities& probabilities);
    std::vector<SoundEvent> Flush(uint64_t endSample);
    void Reset();

    bool IsActive(SoundClass soundClass) const;
    std::array<bool, kSoundClassCount> ActiveClasses() const;
    bool IsAmbient() const;

private:
    struct State {
        bool active{false};
        bool armed{true};
        uint32_t aboveCount{0};
        uint32_t belowCount{0};
        uint64_t candidateOnset{0};
        uint64_t onset{0};
        uint64_t refractoryUntil{0};
        uint64_t activeUntil{0};
        float peak{0.0f};
    };

    Config m_config;
    std::array<State, kSoundClassCount> m_states{};
};

} // namespace EchoRadar
