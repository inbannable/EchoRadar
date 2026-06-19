#pragma once
#include "../common/Types.h"
#include <deque>
#include <functional>

namespace EchoRadar {

/// Detects footstep events from a power spectrogram.
/// Strategy: periodic low-frequency impulse pattern detection.
class FootstepDetector {
public:
    struct Config {
        uint32_t low_freq_bin_min{2};   // ~90 Hz  at 48 kHz / 1024
        uint32_t low_freq_bin_max{20};  // ~940 Hz
        float    impulse_threshold{3.0f}; // ratio above local average
        uint32_t min_period_ms{200};    // fastest plausible step cadence
        uint32_t max_period_ms{800};    // slowest plausible step cadence
    };

    using EventCallback = std::function<void(const FootstepEvent&)>;

    explicit FootstepDetector(Config cfg = {});
    ~FootstepDetector() = default;

    /// Feed a new spectrogram frame; fires callback on detection.
    void Process(const Spectrogram& spec, EventCallback cb = nullptr);

    FootstepEvent LastEvent() const { return m_last; }

    void Reset();

private:
    Config                m_cfg;
    FootstepEvent         m_last;
    std::deque<uint64_t>  m_impulse_times; // recent impulse timestamps (ms)
    float                 m_local_avg{0.0f};
};

} // namespace EchoRadar
