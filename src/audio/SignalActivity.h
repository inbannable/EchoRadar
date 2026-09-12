#pragma once
#include "AudioTypes.h"
#include <algorithm>
#include <cmath>

namespace EchoRadar {
struct SignalActivitySnapshot {
    bool active{false};
    float rmsDbfs{-120.0f};
    float balance{0.0f}; // -1 left, +1 right. Energy balance, never azimuth.
    bool stereoBalanceAvailable{false};
};

// Runs on the DSP thread. Independent of model thresholds and directional energy.
class SignalActivity {
public:
    void Reset() { m_snapshot = {}; m_holdFrames = 0; }
    SignalActivitySnapshot Snapshot() const { return m_snapshot; }
    void Push(const float* pcm, size_t frames, const AudioChannelLayout& layout) {
        if (!pcm || !frames || !layout.IsValid()) return;
        double power = 0.0, left = 0.0, right = 0.0;
        for (size_t frame = 0; frame < frames; ++frame) {
            for (size_t channel = 0; channel < layout.channelCount; ++channel) {
                const float value = pcm[frame * layout.channelCount + channel];
                const double energy = std::isfinite(value)
                    ? static_cast<double>(value) * value : 0.0;
                power += energy;
                if (channel == 0) left += energy;
                if (channel == 1) right += energy;
            }
        }
        const double mean = power / (frames * layout.channelCount);
        m_snapshot.rmsDbfs = static_cast<float>(10.0 * std::log10(std::max(mean, 1e-12)));
        const float threshold = m_snapshot.active ? -72.0f : -66.0f;
        if (m_snapshot.rmsDbfs >= threshold) m_holdFrames = 7200; // 150 ms at 48 kHz
        else m_holdFrames = frames >= m_holdFrames ? 0 : m_holdFrames - frames;
        m_snapshot.active = m_holdFrames != 0;
        m_snapshot.stereoBalanceAvailable = layout.kind == AudioChannelLayoutKind::Stereo;
        m_snapshot.balance = m_snapshot.stereoBalanceAvailable && mean > 1e-12
            ? static_cast<float>((right - left) / std::max(left + right, 1e-12)) : 0.0f;
    }
private:
    SignalActivitySnapshot m_snapshot{};
    size_t m_holdFrames{0};
};
} // namespace EchoRadar
