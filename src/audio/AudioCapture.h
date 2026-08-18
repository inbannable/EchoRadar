#pragma once

#include "AudioDeviceInfo.h"
#include "AudioTypes.h"

#include <cstddef>
#include <memory>
#include <string>

namespace EchoRadar {

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
    AudioEndpointSelection selection{AudioEndpointSelection::FollowDefault};
    std::string endpointId;
    std::string endpointName;
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    size_t bufferFrames{48000};
    size_t maxBacklogFrames{9600};
    size_t retainedFrames{960};
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

/// Pull-based 48 kHz stereo Windows WASAPI loopback capture.
class AudioCapture {
public:
    static constexpr size_t kDefaultBufferFrames = 48000;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool Start(const AudioCaptureConfig& config = {});
    void Poll();
    void Stop();

    bool IsRunning() const;
    AudioCaptureState GetState() const;
    AudioCaptureStatus GetStatus() const;

    size_t GetAvailableFrames() const;
    uint64_t GetDroppedFrames() const;
    size_t ReadInterleaved(float* destination, size_t frameCount);
    AudioReadResult Read(float* destination, size_t frameCount);
    AudioLevels GetCurrentLevels() const;

private:
    bool StartInternal(const AudioCaptureConfig& config);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
