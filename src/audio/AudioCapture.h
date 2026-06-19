#pragma once
#include "../common/Types.h"
#include <functional>
#include <string>

namespace EchoRadar {

/// Captures stereo PCM from a system audio device (e.g. VB-Cable / OBS Monitor).
/// Output: 48 kHz, Stereo, 32-bit float.
class AudioCapture {
public:
    /// Called on the audio thread for every captured frame.
    using FrameCallback = std::function<void(const AudioFrame&)>;

    AudioCapture()  = default;
    ~AudioCapture() = default;

    // Non-copyable, moveable
    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    AudioCapture(AudioCapture&&)                 = default;
    AudioCapture& operator=(AudioCapture&&)      = default;

    /// @param device_name  Partial match against device names; empty = default.
    /// @param callback     Invoked with each captured AudioFrame.
    bool Start(const std::string& device_name = "", FrameCallback callback = nullptr);
    void Stop();

    bool IsRunning() const { return m_running; }

    /// Synchronous pull (alternative to callback).
    /// Returns the most recently completed frame; blocks up to timeout_ms.
    AudioFrame GetFrame(uint32_t timeout_ms = 100);

private:
    bool          m_running{false};
    FrameCallback m_callback;

    // Implementation detail – platform audio handle (miniaudio device)
    struct Impl;
    // std::unique_ptr<Impl> m_impl;   // uncomment in Milestone 1
};

} // namespace EchoRadar
