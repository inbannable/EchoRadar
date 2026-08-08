#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <recognition/PcmWav.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace EchoRadar;

namespace {

std::atomic<bool> g_running{true};
void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }

const char* StateName(AudioCaptureState state) {
    switch (state) {
    case AudioCaptureState::Stopped: return "Stopped";
    case AudioCaptureState::Starting: return "Starting";
    case AudioCaptureState::Running: return "Running";
    case AudioCaptureState::Recovering: return "Recovering";
    case AudioCaptureState::Failed: return "Failed";
    case AudioCaptureState::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

void PrintDevices(AudioCaptureSource source) {
    AudioDeviceManager manager;
    const auto& devices = source == AudioCaptureSource::SystemLoopback
        ? manager.GetOutputDevices() : manager.GetInputDevices();
    std::cout << (source == AudioCaptureSource::SystemLoopback
                      ? "Output endpoints" : "Input endpoints")
              << " (" << devices.size() << "):\n";
    for (const auto& device : devices) {
        std::cout << "  " << device.id << "  " << device.name;
        if (device.isDefault) std::cout << "  <default>";
        if (device.nativeChannels != 0) {
            std::cout << "  " << device.nativeChannels << "ch@" << device.nativeSampleRate;
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    AudioCaptureConfig config;
    bool listOnly = false;
    bool secondsSpecified = false;
    std::filesystem::path recordPath;
    double recordSeconds = 0.0;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--source" && index + 1 < argc) {
            const std::string source(argv[++index]);
            if (source == "loopback") config.source = AudioCaptureSource::SystemLoopback;
            else if (source == "input") config.source = AudioCaptureSource::InputDevice;
            else {
                std::cerr << "--source must be loopback or input\n";
                return 2;
            }
        } else if (argument == "--audio-output-id" && index + 1 < argc) {
            config.selection = AudioEndpointSelection::Fixed;
            config.endpointId = argv[++index];
        } else if (argument == "--device" && index + 1 < argc) {
            config.source = AudioCaptureSource::InputDevice;
            config.selection = AudioEndpointSelection::Fixed;
            config.endpointName = argv[++index];
        } else if (argument == "--list-devices" || argument == "-l") {
            listOnly = true;
        } else if (argument == "--record" && index + 1 < argc) {
            recordPath = argv[++index];
        } else if (argument == "--seconds" && index + 1 < argc) {
            try {
                recordSeconds = std::stod(argv[++index]);
                secondsSpecified = true;
            } catch (...) {
                std::cerr << "--seconds must be a positive number\n";
                return 2;
            }
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: audio_monitor [options]\n\n"
                << "  --source loopback|input    Default: loopback\n"
                << "  --list-devices             List endpoints for the selected source\n"
                << "  --audio-output-id <id>     Pin a loopback render endpoint\n"
                << "  --device <partial-name>    Pin an input device (legacy diagnostics)\n"
                << "  --record <wav>             Save captured PCM16 WAV\n"
                << "  --seconds <n>              Stop after n seconds\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }
    if (secondsSpecified && recordSeconds <= 0.0) {
        std::cerr << "--seconds must be a positive number\n";
        return 2;
    }
    if (listOnly) {
        PrintDevices(config.source);
        return 0;
    }
    if (!recordPath.empty() && recordSeconds <= 0.0) recordSeconds = 10.0;

    AudioCapture capture;
    if (!capture.Start(config)) {
        std::cerr << "Capture setup failed: " << capture.GetStatus().lastError << '\n';
        return 1;
    }
    std::cout << "EchoRadar audio monitor; Ctrl+C to stop\n";
    constexpr size_t kChunkFrames = 480;
    std::vector<float> samples(kChunkFrames * 2);
    std::vector<float> recording;
    const uint64_t targetFrames = recordSeconds > 0.0
        ? static_cast<uint64_t>(recordSeconds * 48000.0) : 0;
    const auto stopAt = recordSeconds > 0.0
        ? std::chrono::steady_clock::now() + std::chrono::duration<double>(recordSeconds)
        : std::chrono::steady_clock::time_point::max();
    auto nextPrint = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        const AudioReadResult read = capture.Read(samples.data(), kChunkFrames);
        if (read.frames != 0 && !recordPath.empty()) {
            recording.insert(recording.end(), samples.begin(),
                             samples.begin() + static_cast<std::ptrdiff_t>(read.frames * 2));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= stopAt) break;
        if (now >= nextPrint) {
            const auto levels = capture.GetCurrentLevels();
            const auto status = capture.GetStatus();
            std::printf("state=%s endpoint=%s L=%.3f R=%.3f buffered=%zu dropped=%llu "
                        "discarded=%llu generation=%llu     \r",
                        StateName(status.state), status.endpointName.c_str(),
                        levels.leftRms, levels.rightRms, capture.GetAvailableFrames(),
                        static_cast<unsigned long long>(status.droppedFrames),
                        static_cast<unsigned long long>(status.discardedBacklogFrames),
                        static_cast<unsigned long long>(status.streamGeneration));
            std::fflush(stdout);
            nextPrint = now + std::chrono::milliseconds(100);
        }
        if (read.frames == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    capture.Stop();
    std::cout << "\nStopped.\n";

    if (!recordPath.empty()) {
        if (targetFrames != 0 && recording.size() / 2 > targetFrames) {
            recording.resize(static_cast<size_t>(targetFrames) * 2);
        }
        PcmAudio audio;
        audio.sampleRate = 48000;
        audio.channels = 2;
        audio.interleaved = std::move(recording);
        std::string error;
        if (!WritePcm16Wav(recordPath, audio, &error)) {
            std::cerr << "Could not write recording: " << error << '\n';
            return 1;
        }
        std::cout << "Wrote " << recordPath.string() << '\n';
    }
    return 0;
}
