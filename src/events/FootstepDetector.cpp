#include "FootstepDetector.h"

namespace EchoRadar {

FootstepDetector::FootstepDetector(Config cfg) : m_cfg(cfg) {}

void FootstepDetector::Process(const Spectrogram& spec, EventCallback cb) {
    if (spec.magnitude.empty()) return;

    // TODO (Milestone 5): detect low-frequency periodic impulses.
    // Stub: never fires.
    (void)cb;
}

void FootstepDetector::Reset() {
    m_last       = {};
    m_impulse_times.clear();
    m_local_avg  = 0.0f;
}

} // namespace EchoRadar
