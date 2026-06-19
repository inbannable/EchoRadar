#include "GunshotDetector.h"
#include <cmath>
#include <numeric>

namespace EchoRadar {

GunshotDetector::GunshotDetector(Config cfg) : m_cfg(cfg) {}

void GunshotDetector::Process(const Spectrogram& spec, EventCallback cb) {
    if (spec.magnitude.empty()) return;

    // TODO (Milestone 4): compute broadband energy, compare to rolling average.
    // Stub: never fires – confidence always 0.
    (void)cb;
}

void GunshotDetector::Reset() {
    m_last         = {};
    m_rolling_avg  = 0.0f;
    m_last_event_ts = 0;
}

} // namespace EchoRadar
