#include "DirectionSceneCoordinator.h"

#include <algorithm>
#include <stdexcept>

namespace EchoRadar {

DirectionSceneCoordinator::DirectionSceneCoordinator()
    : DirectionSceneCoordinator(Config{}) {}

DirectionSceneCoordinator::DirectionSceneCoordinator(Config config)
    : m_config(config) {
    if (m_config.sampleRate == 0 || m_config.sceneSamples == 0 ||
        m_config.joinMilliseconds == 0) {
        throw std::invalid_argument("Direction scene coordinator configuration is invalid");
    }
}

uint64_t DirectionSceneCoordinator::AddEvent(uint64_t eventId,
                                              const SoundEvent& event) {
    if (!m_haveGeneration || event.streamGeneration != m_streamGeneration) {
        Reset(event.streamGeneration);
    }
    const uint64_t joinSamples = static_cast<uint64_t>(m_config.joinMilliseconds) *
        m_config.sampleRate / 1000u;
    for (auto iterator = m_pending.begin(); iterator != m_pending.end(); ++iterator) {
        if (iterator->streamGeneration != event.streamGeneration) continue;
        const uint64_t prospectiveFirst = std::min(
            iterator->firstOnsetSample, event.onsetSample);
        const uint64_t prospectiveLast = std::max(
            iterator->lastOnsetSample, event.onsetSample);
        if (prospectiveLast - prospectiveFirst <= joinSamples) {
            if (event.onsetSample < iterator->firstOnsetSample) {
                iterator->firstOnsetSample = event.onsetSample;
                const uint64_t preAnchorSamples =
                    static_cast<uint64_t>(m_config.preAnchorMilliseconds) *
                    m_config.sampleRate / 1000u;
                iterator->sceneStartSample = event.onsetSample > preAnchorSamples
                    ? event.onsetSample - preAnchorSamples : 0;
                iterator->sceneEndSample = iterator->sceneStartSample + m_config.sceneSamples;
            }
            iterator->lastOnsetSample = prospectiveLast;
            iterator->events.push_back({eventId, event});
            std::stable_sort(iterator->events.begin(), iterator->events.end(),
                [](const DirectionSceneEvent& left, const DirectionSceneEvent& right) {
                    if (left.event.onsetSample != right.event.onsetSample) {
                        return left.event.onsetSample < right.event.onsetSample;
                    }
                    return left.eventId < right.eventId;
                });
            const uint64_t sceneId = iterator->sceneId;
            std::stable_sort(m_pending.begin(), m_pending.end(),
                [](const PendingScene& left, const PendingScene& right) {
                    return left.firstOnsetSample < right.firstOnsetSample;
                });
            return sceneId;
        }
    }

    const uint64_t preAnchorSamples = static_cast<uint64_t>(m_config.preAnchorMilliseconds) *
        m_config.sampleRate / 1000u;
    const uint64_t start = event.onsetSample > preAnchorSamples
        ? event.onsetSample - preAnchorSamples : 0;
    PendingScene scene;
    scene.sceneId = m_nextSceneId++;
    scene.streamGeneration = event.streamGeneration;
    scene.firstOnsetSample = event.onsetSample;
    scene.lastOnsetSample = event.onsetSample;
    scene.sceneStartSample = start;
    scene.sceneEndSample = start + m_config.sceneSamples;
    scene.events.push_back({eventId, event});
    const uint64_t sceneId = scene.sceneId;
    const auto insertion = std::upper_bound(
        m_pending.begin(), m_pending.end(), scene.firstOnsetSample,
        [](uint64_t onset, const PendingScene& existing) {
            return onset < existing.firstOnsetSample;
        });
    m_pending.insert(insertion, std::move(scene));
    return sceneId;
}

std::vector<DirectionSceneJob> DirectionSceneCoordinator::TakeReady(
    uint64_t newestSampleExclusive,
    uint64_t oldestSample) {
    std::vector<DirectionSceneJob> ready;
    while (!m_pending.empty() && m_pending.front().sceneEndSample <= newestSampleExclusive) {
        PendingScene scene = std::move(m_pending.front());
        m_pending.pop_front();
        DirectionSceneJob job;
        job.sceneId = scene.sceneId;
        job.streamGeneration = scene.streamGeneration;
        job.anchorEventSample = scene.firstOnsetSample;
        job.sceneStartSample = scene.sceneStartSample;
        job.sceneEndSample = scene.sceneEndSample;
        job.audioAvailable = scene.sceneStartSample >= oldestSample;
        job.events = std::move(scene.events);
        ready.push_back(std::move(job));
    }
    return ready;
}

void DirectionSceneCoordinator::Reset(uint64_t streamGeneration) {
    m_pending.clear();
    m_streamGeneration = streamGeneration;
    m_haveGeneration = true;
}

} // namespace EchoRadar

