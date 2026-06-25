#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <dsp/STFTProcessor.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace EchoRadar;

static std::atomic<bool> g_running{true};

static void OnSignal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

struct ChannelSummary {
    float peakHz{0.0f};
    float energy{0.0f};
};

static ChannelSummary SummarizeChannel(const STFTChannelFrame& channel, float hzPerBin) {
    ChannelSummary summary{};
    if (channel.power.empty()) {
        return summary;
    }

    uint32_t peakBin = 0;
    float maxMag = 0.0f;
    for (uint32_t i = 1; i < channel.magnitudes.size(); ++i) {
        if (channel.magnitudes[i] > maxMag) {
            maxMag = channel.magnitudes[i];
            peakBin = i;
        }
    }

    summary.peakHz = static_cast<float>(peakBin) * hzPerBin;
    summary.energy = std::accumulate(channel.power.begin(), channel.power.end(), 0.0f);
    return summary;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
#ifndef _WIN32
    std::signal(SIGTERM, OnSignal);
#endif

    std::string deviceArg;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--list-devices" || arg == "-l") {
            listOnly = true;
        } else if (arg == "--device" || arg == "-d") {
            if (i + 1 < argc) {
                deviceArg = argv[++i];
            } else {
                std::cerr << "[Error] --device requires a device name argument\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: stft_monitor [options]\n\n"
                << "Options:\n"
                << "  --list-devices              List available input devices\n"
                << "  --device <name>             Capture from a specific device (case-insensitive match)\n"
                << "  --help                      Show this help message\n";
            return 0;
        }
    }

    AudioDeviceManager mgr;
    const auto devices = mgr.GetInputDevices();
    if (listOnly) {
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

    std::cout << "[EchoRadar STFT Monitor]\n";
    if (!deviceArg.empty()) {
        const auto found = mgr.FindInputDeviceByName(deviceArg);
        if (found) {
            std::cout << "Using device: " << found->name << "\n\n";
        }
        started = capture.StartDeviceByName(deviceArg);
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

    STFTProcessor processor;
    constexpr size_t kReadChunkFrames = 480;
    std::vector<float> interleaved(kReadChunkFrames * 2, 0.0f);

    uint64_t totalFrames = 0;
    ChannelSummary leftSummary{};
    ChannelSummary rightSummary{};

    std::cout << "Press Ctrl+C to stop.\n\n";

    while (g_running.load(std::memory_order_relaxed) && capture.IsRunning()) {
        size_t pulled = 0;
        do {
            pulled = capture.ReadInterleaved(interleaved.data(), kReadChunkFrames);
            if (pulled > 0) {
                processor.PushInterleaved(interleaved.data(), pulled);
            }
        } while (pulled == kReadChunkFrames);

        STFTFrame frame;
        while (processor.PopFrame(frame)) {
            ++totalFrames;
            const float hzPerBin = static_cast<float>(frame.sample_rate) /
                                   static_cast<float>(frame.fft_size);
            leftSummary = SummarizeChannel(frame.left, hzPerBin);
            rightSummary = SummarizeChannel(frame.right, hzPerBin);
        }

        std::printf(
            "Frames: %-8llu  Left peak: %8.1f Hz  Right peak: %8.1f Hz  "
            "Left energy: %10.3f  Right energy: %10.3f  Buffered PCM: %6zu fr  Buffered STFT: %3zu\r",
            static_cast<unsigned long long>(totalFrames),
            leftSummary.peakHz,
            rightSummary.peakHz,
            leftSummary.energy,
            rightSummary.energy,
            capture.GetAvailableFrames(),
            processor.GetAvailableSTFTFrames());
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::printf("\nStopping...\n");
    capture.Stop();
    return 0;
}
