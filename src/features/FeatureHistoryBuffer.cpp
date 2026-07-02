#include "FeatureHistoryBuffer.h"
#include <stdexcept>

namespace EchoRadar {

FeatureHistoryBuffer::FeatureHistoryBuffer(double historySeconds)
    : m_historySeconds(historySeconds) {
    if (m_historySeconds <= 0.0) {
        throw std::invalid_argument("FeatureHistoryBuffer historySeconds must be > 0");
    }
}

void FeatureHistoryBuffer::Reset() {
    m_entries.clear();
}

void FeatureHistoryBuffer::Push(const AudioFeatures& features,
                                double timeSec,
                                int frameIndex,
                                float candidateScore,
                                float confidence) {
    FeatureHistoryEntry e{};
    e.features = features;
    e.timeSec = timeSec;
    e.frameIndex = frameIndex;
    e.candidateScore = candidateScore;
    e.confidence = confidence;
    m_entries.push_back(e);

    const double minTime = timeSec - m_historySeconds;
    while (!m_entries.empty() && m_entries.front().timeSec < minTime) {
        m_entries.pop_front();
    }
}

std::vector<FeatureHistoryEntry> FeatureHistoryBuffer::ExtractWindow(double startTimeSec, double endTimeSec) const {
    std::vector<FeatureHistoryEntry> out;
    if (endTimeSec < startTimeSec) {
        return out;
    }

    for (const FeatureHistoryEntry& entry : m_entries) {
        if (entry.timeSec < startTimeSec) {
            continue;
        }
        if (entry.timeSec > endTimeSec) {
            break;
        }
        out.push_back(entry);
    }
    return out;
}

double FeatureHistoryBuffer::GetOldestTimeSec() const {
    if (m_entries.empty()) {
        return 0.0;
    }
    return m_entries.front().timeSec;
}

double FeatureHistoryBuffer::GetNewestTimeSec() const {
    if (m_entries.empty()) {
        return 0.0;
    }
    return m_entries.back().timeSec;
}

} // namespace EchoRadar
