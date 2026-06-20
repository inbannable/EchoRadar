#pragma once
#include "../common/Types.h"
#include "AudioDeviceInfo.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace EchoRadar {

/// Real-time stereo audio capture via miniaudio.
/// Output format: 48 kHz · stereo · float32.
///
/// Pull model (recommended for DSP / monitoring):
///   AudioCapture cap;
///   cap.StartDefault();                     // or StartDeviceByName("CABLE Output")
///   while (running) {
///       float buf[960];                      // 10 ms @ 48 kHz stereo = 480 frames × 2 ch
///       cap.ReadInterleaved(buf, 480);
///       AudioLevels lvl = cap.GetCurrentLevels();
///   }
///   cap.Stop();
///
/// Callback model (legacy — used by EchoRadarApp):
///   cap.Start("", [](const AudioFrame& f){ ring.Push(f); });
class AudioCapture {
public:
    /// Internal ring buffer holds this many stereo frames (4 s @ 48 kHz).
    static constexpr size_t kDefaultBufferFrames = 48000 * 4;

    /// Invoked (on the callback thread) for each decoded AudioFrame — legacy path.
    using FrameCallback = std::function<void(const AudioFrame&)>;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // ── Capture control ───────────────────────────────────────────────────────

    /// Open the system default input device.
    bool StartDefault();

    /// Open the first device whose name contains @p name (case-insensitive).
    /// Falls back to default if no match is found.
    bool StartDeviceByName(const std::string& name);

    /// Legacy entry-point (EchoRadarApp compat).
    /// @p device_name empty → default device.
    /// @p callback non-null → called on the callback thread per captured AudioFrame.
    bool Start(const std::string& device_name = "", FrameCallback callback = nullptr);

    /// Stop capture and release the audio device.
    void Stop();

    bool IsRunning() const;

    // ── Buffer access — pull model ────────────────────────────────────────────

    /// Number of stereo frames currently in the internal ring buffer.
    size_t GetAvailableFrames() const;

    /// Read up to @p frameCount interleaved stereo float32 frames into @p dst.
    /// @return Number of frames actually copied.
    size_t ReadInterleaved(float* dst, size_t frameCount);

    // ── Level monitoring ─────────────────────────────────────────────────────

    /// Per-channel RMS and peak computed inside the last audio callback block.
    AudioLevels GetCurrentLevels() const;

    // ── Legacy synchronous pull ──────────────────────────────────────────────

    /// Block up to @p timeout_ms for one 10-ms AudioFrame (480 samples @ 48 kHz).
    /// Returns a silent frame on timeout.
    AudioFrame GetFrame(uint32_t timeout_ms = 100);

private:
    /// Shared implementation for all Start* variants.
    /// @p deviceName nullptr → default device; non-null → partial name search.
    bool StartInternal(const char* deviceName);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
