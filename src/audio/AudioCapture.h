#pragma once
#include "../common/Types.h"
#include "AudioDeviceInfo.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace EchoRadar {

enum class AudioCaptureSource {
    SystemLoopback,
    InputDevice,
};

enum class AudioEndpointSelection {
    FollowDefault,
    Fixed,
};

enum class AudioCaptureState {
    Stopped,
    Starting,
    Running,
    Recovering,
    Failed,
    Unsupported,
};

struct AudioCaptureConfig {
    AudioCaptureSource source{AudioCaptureSource::SystemLoopback};
    AudioEndpointSelection selection{AudioEndpointSelection::FollowDefault};
    std::string endpointId;
    std::string endpointName;
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    size_t bufferFrames{48000};
    size_t maxBacklogFrames{9600};   ///< 200 ms at 48 kHz
    size_t retainedFrames{960};      ///< keep latest 20 ms after catch-up
    uint32_t endpointPollMs{1000};
};

struct AudioReadResult {
    size_t frames{0};
    uint64_t firstSample{0};
    uint64_t streamGeneration{0};
    bool discontinuity{false};
};

struct AudioCaptureStatus {
    AudioCaptureState state{AudioCaptureState::Stopped};
    std::string endpointId;
    std::string endpointName;
    std::string lastError;
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    uint32_t nativeChannels{0};
    uint32_t nativeSampleRate{0};
    uint64_t streamGeneration{0};
    uint64_t droppedFrames{0};
    uint64_t discardedBacklogFrames{0};
    uint64_t restartCount{0};
};

/// Real-time stereo audio capture via miniaudio.
/// Output format: 48 kHz · stereo · float32.
///
/// Pull model (recommended for the application and diagnostics):
///   AudioCapture cap;
///   cap.Start(AudioCaptureConfig{});         // Windows default-output loopback
///   while (running) {
///       float buf[960];                      // 10 ms @ 48 kHz stereo = 480 frames × 2 ch
///       const auto read = cap.Read(buf, 480);
///       AudioLevels lvl = cap.GetCurrentLevels();
///   }
///   cap.Stop();
///
/// Callback model (legacy input-device tools only):
///   cap.Start("", [](const AudioFrame& f){ ring.Push(f); });
///
/// Start(), Poll(), Read(), Stop(), and GetStatus() form one control/consumer
/// thread. The miniaudio callback is the sole producer of the internal ring.
class AudioCapture {
public:
    /// Default internal buffer is one second. The consumer catches up before it
    /// can become a source of delayed recognition events.
    static constexpr size_t kDefaultBufferFrames = 48000;

    /// Invoked (on the callback thread) for each decoded AudioFrame — legacy path.
    using FrameCallback = std::function<void(const AudioFrame&)>;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // ── Capture control ───────────────────────────────────────────────────────

    /// Start an explicit capture route. A recoverable missing endpoint leaves
    /// the object in Recovering state and returns true; Poll() will retry.
    bool Start(const AudioCaptureConfig& config);

    /// Handle endpoint changes and retry recoverable failures. Call from the
    /// same consumer/control thread that calls Read().
    void Poll();

    /// Open the system default input device.
    bool StartDefault();

    /// Open the first device whose name contains @p name (case-insensitive).
    /// Falls back to default if no match is found.
    bool StartDeviceByName(const std::string& name);

    /// Legacy entry-point for input-device tools.
    /// @p device_name empty → default device.
    /// @p callback non-null → called on the callback thread per captured AudioFrame.
    bool Start(const std::string& device_name = "", FrameCallback callback = nullptr);

    /// Stop capture and release the audio device.
    void Stop();

    bool IsRunning() const;
    AudioCaptureState GetState() const;
    AudioCaptureStatus GetStatus() const;

    // ── Buffer access — pull model ────────────────────────────────────────────

    /// Number of stereo frames currently in the internal ring buffer.
    size_t GetAvailableFrames() const;

    /// Total input frames dropped because the callback ring buffer was full.
    uint64_t GetDroppedFrames() const;

    /// Read up to @p frameCount interleaved stereo float32 frames into @p dst.
    /// @return Number of frames actually copied.
    size_t ReadInterleaved(float* dst, size_t frameCount);

    /// Read PCM plus stream-continuity metadata.
    AudioReadResult Read(float* dst, size_t frameCount);

    // ── Level monitoring ─────────────────────────────────────────────────────

    /// Per-channel RMS and peak computed inside the last audio callback block.
    AudioLevels GetCurrentLevels() const;

    // ── Legacy synchronous pull ──────────────────────────────────────────────

    /// Block up to @p timeout_ms for one 10-ms AudioFrame (480 samples @ 48 kHz).
    /// Returns a silent frame on timeout.
    AudioFrame GetFrame(uint32_t timeout_ms = 100);

private:
    bool StartInternal(const AudioCaptureConfig& config, FrameCallback callback);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
