/// tools/audio_monitor/main.cpp
/// ─────────────────────────────────────────────────────────────────────────────
/// EchoRadar Audio Monitor
///
/// Usage:
///   audio_monitor                            capture from default input device
///   audio_monitor --list-devices             print available input devices
///   audio_monitor --device "CABLE Output"    capture from a named device
///
/// Prints live RMS / peak levels every 100 ms until Ctrl-C is pressed.
/// ─────────────────────────────────────────────────────────────────────────────
#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

using namespace EchoRadar;

// ── Globals ──────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }

static void OnSegmentationFault(int sig) {
    std::cerr << "\n[FATAL] Segmentation fault (signal " << sig << ") - access violation!\n" << std::flush;
    std::exit(1);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

/// ASCII bar chart for an RMS / peak value in [0, 1].
static std::string Bar(float value, int width = 20) {
    const int filled = std::min(static_cast<int>(value * width), width);
    return std::string(filled, '#') + std::string(width - filled, '-');
}

static void PrintDeviceList(const std::vector<AudioDeviceInfo>& devs, bool detailed = false) {
    std::cout << "[EchoRadar] Available input devices (" << devs.size() << "):\n";
    if (devs.empty()) {
        std::cout << "  (none found — is an audio driver installed?)\n";
        return;
    }
    
    if (detailed) {
        // Detailed list with device types for debugging
        for (size_t i = 0; i < devs.size(); ++i) {
            std::cout << "  [" << i << "] " << devs[i].name << "\n"
                      << "       Type: " << DeviceTypeString(devs[i].type);
            if (devs[i].isDefault) std::cout << "  [SYSTEM DEFAULT]";
            std::cout << '\n';
        }
    } else {
        // Simple list with type indicators
        for (size_t i = 0; i < devs.size(); ++i) {
            std::cout << "  [" << i << "] " << devs[i].name;
            if (devs[i].isDefault) std::cout << "  <default>";
            // Add type indicator if not microphone
            if (devs[i].type == AudioDeviceType::Loopback) {
                std::cout << "  [GAME AUDIO]";
            } else if (devs[i].type == AudioDeviceType::VirtualCable) {
                std::cout << "  [VIRTUAL CABLE]";
            }
            std::cout << '\n';
        }
    }
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Disable sync with C stdio to allow immediate flushing
    std::ios::sync_with_stdio(false);
    
    std::signal(SIGINT,  OnSignal);
#ifndef _WIN32
    // SIGTERM may be problematic on Windows
    std::signal(SIGTERM, OnSignal);
    // Catch segmentation faults on Unix
    std::signal(SIGSEGV, OnSegmentationFault);
#else
    // On Windows, we cannot directly catch SIGSEGV the same way,
    // but we can at least try to catch structured exceptions
    std::signal(SIGABRT, OnSegmentationFault);
#endif

    // ── Parse arguments ───────────────────────────────────────────────────────
    std::string deviceArg;
    bool        listOnly = false;
    bool        detailedList = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--list-devices" || arg == "-l") {
            listOnly = true;
        } else if (arg == "--list-devices-detailed" || arg == "--list-detailed") {
            listOnly = true;
            detailedList = true;
        } else if (arg == "--device" || arg == "-d") {
            if (i + 1 < argc) {
                deviceArg = argv[++i];
            } else {
                std::cerr << "[Error] --device requires a device name argument\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: audio_monitor [options]\n\n"
                "Options:\n"
                "  --list-devices              List available input devices\n"
                "  --list-devices-detailed     Show device types (microphone, stereo mix, etc.)\n"
                "  --device <name>             Capture from a specific device (case-insensitive match)\n"
                "  --help                      Show this help message\n\n"
                "Examples:\n"
                "  audio_monitor                     Use default device\n"
                "  audio_monitor --list-devices      List all devices\n"
                "  audio_monitor --device \"CABLE\"   Capture from VB-Cable\n"
                "  audio_monitor --device \"mix\"     Capture from Stereo Mix\n"
                "\nFor CS2 gunshot detection, use Stereo Mix or Virtual Cable for game audio.\n";
            return 0;
        }
    }

    // ── Enumerate devices ─────────────────────────────────────────────────────
    AudioDeviceManager mgr;
    const auto& devs = mgr.GetInputDevices();

    if (listOnly) {
        PrintDeviceList(devs, detailedList);
        return 0;
    }

    if (devs.empty()) {
        std::cerr << "[Warning] No input devices found.\n";
        PrintDeviceList(devs, false);
        return 1;
    }

    // ── Start capture ─────────────────────────────────────────────────────────
    std::cout << "[EchoRadar Audio Monitor]\n";

    AudioCapture capture;
    bool started = false;

    if (!deviceArg.empty()) {
        const auto found = mgr.FindInputDeviceByName(deviceArg);
        if (found) {
            std::cout << "Using device: " << found->name << " (" << DeviceTypeString(found->type) << ")\n\n";
        } else {
            std::cerr << "[Warning] Device '" << deviceArg
                      << "' not found. Falling back to default.\n\n";
        }
        started = capture.StartDeviceByName(deviceArg);
    }

    if (!started) {
        const auto def = mgr.GetDefaultInputDevice();
        std::cout << "Using device: " << (def ? def->name : "default");
        if (def) {
            std::cout << " (" << DeviceTypeString(def->type) << ")";
        }
        std::cout << "\n\n";
        started = capture.StartDefault();
    }

    if (!started) {
        std::cerr << "[Error] Failed to start audio capture.\n";
        return 1;
    }

    std::cout << "Press Ctrl+C to stop.\n\n";

    // ── Monitor loop ──────────────────────────────────────────────────────────
    constexpr int kInterval = 100; // ms
    int iterations = 0;
    
    try {
        while (g_running.load(std::memory_order_relaxed)) {
            ++iterations;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(kInterval));

            if (!capture.IsRunning()) {
                break;
            }

            const AudioLevels lvl      = capture.GetCurrentLevels();
            const size_t      buffered = capture.GetAvailableFrames();

            printf("[%4d] L RMS: %6.3f  R RMS: %6.3f  L Peak: %6.3f  R Peak: %6.3f  Buf: %6zu fr\n",
                   iterations, lvl.leftRms, lvl.rightRms, lvl.leftPeak, lvl.rightPeak, buffered);
            fflush(stdout);
        }
    } catch (const std::exception& e) {
        std::cerr << "[error] Exception in monitor loop: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[error] Unknown exception in monitor loop\n";
    }
    
    printf("Exiting normally without calling Stop()\n");
    fflush(stdout);

    std::cout << "\nStopping...\n";
    capture.Stop();
    return 0;
}
