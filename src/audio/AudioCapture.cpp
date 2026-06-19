#include "AudioCapture.h"
#include <iostream>

namespace EchoRadar {

bool AudioCapture::Start(const std::string& device_name, FrameCallback callback) {
    if (m_running) return true;
    m_callback = std::move(callback);
    m_running  = true;
    // TODO (Milestone 1): initialise miniaudio device and start capture loop.
    std::cout << "[AudioCapture] Started (stub) device='"
              << (device_name.empty() ? "default" : device_name) << "'\n";
    return true;
}

void AudioCapture::Stop() {
    if (!m_running) return;
    m_running = false;
    // TODO (Milestone 1): stop and uninitialise miniaudio device.
    std::cout << "[AudioCapture] Stopped\n";
}

AudioFrame AudioCapture::GetFrame(uint32_t /*timeout_ms*/) {
    // TODO (Milestone 1): block until a frame is available.
    AudioFrame frame;
    frame.sample_rate = 48000;
    frame.left .assign(480, 0.0f); // 10 ms of silence
    frame.right.assign(480, 0.0f);
    return frame;
}

} // namespace EchoRadar
