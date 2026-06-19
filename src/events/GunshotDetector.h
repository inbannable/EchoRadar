#pragma once
#include "../common/Types.h"
#include <functional>

namespace EchoRadar {

/// Detects gunshot events from a power spectrogram.
/// Strategy: instantaneous broadband energy spike above adaptive threshold.
class GunshotDetector {
public:
    struct Config {
        float    threshold_db{15.0f};   // energy rise above rolling average (dB)
        uint32_t cooldown_ms{200};      // minimum ms between successive events
        float    min_confidence{0.5f};
    };

    using EventCallback = std::function<void(const GunshotEvent&)>;

    explicit GunshotDetector(Config cfg = {});
    ~GunshotDetector() = default;

    /// Feed a new spectrogram frame; fires callback on detection.
    void Process(const Spectrogram& spec, EventCallback cb = nullptr);

    /// Returns the most recent detection (zero-confidence if none yet).
    GunshotEvent LastEvent() const { return m_last; }

    void Reset();

private:
    Config       m_cfg;
    GunshotEvent m_last;
    float        m_rolling_avg{0.0f};
    uint64_t     m_last_event_ts{0};
};

} // namespace EchoRadar
