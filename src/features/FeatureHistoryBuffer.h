#pragma once
#include "../common/Types.h"
#include <deque>
#include <vector>

namespace EchoRadar {

struct FeatureHistoryEntry {
    AudioFeatures features{};
    double timeSec{0.0};
    int frameIndex{0};
    float candidateScore{0.0f};
    float confidence{0.0f};
};

/// Keeps a rolling feature history for fast CSV export around event time.
class FeatureHistoryBuffer {
public:
    explicit FeatureHistoryBuffer(double historySeconds = 3.0);

    void Reset();
    void Push(const AudioFeatures& features,
              double timeSec,
              int frameIndex,
              float candidateScore,
              float confidence);

    std::vector<FeatureHistoryEntry> ExtractWindow(double startTimeSec, double endTimeSec) const;

    size_t Size() const { return m_entries.size(); }
    double GetOldestTimeSec() const;
    double GetNewestTimeSec() const;
    double GetHistorySeconds() const { return m_historySeconds; }

private:
    double m_historySeconds{3.0};
    std::deque<FeatureHistoryEntry> m_entries;
};

} // namespace EchoRadar
