#pragma once

#include <recognition/RecognitionTypes.h>

#include <cstdint>
#include <deque>
#include <vector>

namespace EchoRadar {

struct DirectionSceneEvent {
    uint64_t eventId{0};
    SoundEvent event;
};

struct DirectionSceneJob {
    uint64_t sceneId{0};
    uint64_t streamGeneration{0};
    uint64_t anchorEventSample{0};
    uint64_t sceneStartSample{0};
    uint64_t sceneEndSample{0};
    bool audioAvailable{false};
    std::vector<DirectionSceneEvent> events;
};

/// Clusters accepted events into one fixed scene inference window.
class DirectionSceneCoordinator {
public:
    struct Config {
        uint32_t sampleRate{48000};
        uint32_t preAnchorMilliseconds{40};
        uint32_t joinMilliseconds{120};
        uint32_t sceneSamples{12304};
    };

    DirectionSceneCoordinator();
    explicit DirectionSceneCoordinator(Config config);

    uint64_t AddEvent(uint64_t eventId, const SoundEvent& event);
    std::vector<DirectionSceneJob> TakeReady(uint64_t newestSampleExclusive,
                                             uint64_t oldestSample);
    void Reset(uint64_t streamGeneration);

    size_t PendingSceneCount() const { return m_pending.size(); }
    uint64_t StreamGeneration() const { return m_streamGeneration; }
    const Config& GetConfig() const { return m_config; }

private:
    struct PendingScene {
        uint64_t sceneId{0};
        uint64_t streamGeneration{0};
        uint64_t firstOnsetSample{0};
        uint64_t lastOnsetSample{0};
        uint64_t sceneStartSample{0};
        uint64_t sceneEndSample{0};
        std::vector<DirectionSceneEvent> events;
    };

    Config m_config;
    std::deque<PendingScene> m_pending;
    uint64_t m_streamGeneration{0};
    uint64_t m_nextSceneId{1};
    bool m_haveGeneration{false};
};

} // namespace EchoRadar

