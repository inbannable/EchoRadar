#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <dsp/STFTProcessor.h>
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
                << "Usage: feature_monitor [options]\n\n"
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

    std::cout << "[EchoRadar Feature Monitor]\n";
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

    constexpr size_t kReadChunkFrames = 480;
    std::vector<float> interleaved(kReadChunkFrames * 2, 0.0f);

    uint64_t stft_frame_count = 0;
    AudioFeatures latest{};

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
            latest = extractor.Extract(frame);
            ++stft_frame_count;
        }

        std::printf(
            "Frames: %-7llu  Energy: %9.3f  LogE: %6.3f  Rise: %6.3f  Flux: %6.3f  "
            "HF: %6.3f  Low: %8.3f  Mid: %8.3f  High: %8.3f  Centroid: %8.1f Hz  "
            "Flatness: %.3f  Transient: %.3f  L/R: %+0.3f  PCM: %6zu\r",
            static_cast<unsigned long long>(stft_frame_count),
            latest.totalEnergy,
            latest.logEnergy,
            latest.energyRise,
            latest.spectralFlux,
            latest.hfEnergyRatio,
            latest.lowBandEnergy,
            latest.midBandEnergy,
            latest.highBandEnergy,
            latest.spectralCentroid,
            latest.spectralFlatness,
            latest.transientScore,
            latest.leftRightBalance,
            capture.GetAvailableFrames());
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::printf("\nStopping...\n");
    capture.Stop();
    return 0;
}
