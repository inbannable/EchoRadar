#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <dsp/STFTProcessor.h>
#include <detector/GunshotEventDetector.h>
#include <features/FeatureExtractor.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace EchoRadar;

static std::atomic<bool> g_running{true};

static void OnSignal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

static const char* StateToString(DetectorState state) {
    switch (state) {
    case DetectorState::Idle: return "Idle";
    case DetectorState::InCandidate: return "InCandidate";
    case DetectorState::Cooldown: return "Cooldown";
    }
    return "Unknown";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
#ifndef _WIN32
    std::signal(SIGTERM, OnSignal);
#endif

    std::string device_arg;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--list-devices" || arg == "-l") {
            list_only = true;
        } else if (arg == "--device" || arg == "-d") {
            if (i + 1 < argc) {
                device_arg = argv[++i];
            } else {
                std::cerr << "[Error] --device requires a device name argument\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: gunshot_monitor [options]\n\n"
                << "Options:\n"
                << "  --list-devices              List available input devices\n"
                << "  --device <name>             Capture from a specific device (case-insensitive match)\n"
                << "  --help                      Show this help message\n";
            return 0;
        }
    }

    AudioDeviceManager mgr;
    const auto devices = mgr.GetInputDevices();
    if (list_only) {
        std::cout << "[EchoRadar] Available input devices (" << devices.size() << "):\n";
        for (size_t i = 0; i < devices.size(); ++i) {
            std::cout << "  [" << i << "] " << devices[i].name;
            if (devices[i].isDefault) {
                std::cout << "  <default>";
            }
            std::cout << '\n';
        }
        return 0;
    }

    if (devices.empty()) {
        std::cerr << "[Error] No input devices available.\n";
        return 1;
    }

    AudioCapture capture;
    bool started = false;

    std::cout << "[Gunshot Monitor]\n";
    if (!device_arg.empty()) {
        const auto found = mgr.FindInputDeviceByName(device_arg);
        if (found) {
            std::cout << "Using device: " << found->name << "\n\n";
        }
        started = capture.StartDeviceByName(device_arg);
    }

    if (!started) {
        const auto def = mgr.GetDefaultInputDevice();
        std::cout << "Using device: " << (def ? def->name : "default") << "\n\n";
        started = capture.StartDefault();
    }

    if (!started) {
        std::cerr << "[Error] Failed to start audio capture.\n";
        return 1;
    }

    STFTProcessor stft;
    FeatureExtractor extractor;
    GunshotEventDetector detector;

    constexpr size_t kReadChunkFrames = 480;
    std::vector<float> interleaved(kReadChunkFrames * 2, 0.0f);

    uint64_t stft_frame_count = 0;
    uint64_t event_count = 0;

    std::cout << "Press Ctrl+C to stop.\n\n";

    while (g_running.load(std::memory_order_relaxed) && capture.IsRunning()) {
        size_t pulled = 0;
        do {
            pulled = capture.ReadInterleaved(interleaved.data(), kReadChunkFrames);
            if (pulled > 0) {
                stft.PushInterleaved(interleaved.data(), pulled);
            }
        } while (pulled == kReadChunkFrames);

        STFTFrame frame;
        while (stft.PopFrame(frame)) {
            const AudioFeatures features = extractor.Extract(frame);
            const double frameTimeSec = static_cast<double>(frame.start_sample) /
                                        static_cast<double>(frame.sample_rate);
            detector.PushFrame(features, frameTimeSec, static_cast<int>(frame.frame_index));
            ++stft_frame_count;
        }

        GunshotEvent ev;
        while (detector.PopEvent(ev)) {
            ++event_count;
            const double durationSec = (ev.endFrame >= ev.startFrame)
                ? static_cast<double>(ev.endFrame - ev.startFrame + 1) * 512.0 / 48000.0
                : 0.0;
            const double endTimeSec = static_cast<double>(ev.endFrame) * 512.0 / 48000.0;

            std::printf("\n[GunshotEvent]\n");
            std::printf("onset=%.3fs peak=%.3fs end=%.3fs duration=%.3fs\n",
                        ev.onsetTimeSec, ev.peakTimeSec, endTimeSec, durationSec);
            std::printf("peakScore=%.3f prob=%.3f lr=%+.3f centroid=%.1f hfRatio=%.3f\n",
                        ev.candidateScore, ev.gunshotProb, ev.leftRightBalanceAtPeak,
                        ev.spectralCentroidAtPeak, ev.hfEnergyRatioAtPeak);
        }

        std::printf(
            "Frames: %-7llu  State: %-11s  Score: %.3f  Events: %-4llu  PCM: %6zu\r",
            static_cast<unsigned long long>(stft_frame_count),
            StateToString(detector.GetState()),
            detector.GetLastScore(),
            static_cast<unsigned long long>(event_count),
            capture.GetAvailableFrames());
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::printf("\nStopping...\n");
    capture.Stop();
    return 0;
}
