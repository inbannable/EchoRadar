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
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

using namespace EchoRadar;

// ── Globals ──────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Helpers ───────────────────────────────────────────────────────────────────

/// ASCII bar chart for an RMS / peak value in [0, 1].
static std::string Bar(float value, int width = 20) {
    const int filled = std::min(static_cast<int>(value * width), width);
    return std::string(filled, '#') + std::string(width - filled, '-');
}

static void PrintDeviceList(const std::vector<AudioDeviceInfo>& devs) {
    std::cout << "[EchoRadar] Available input devices (" << devs.size() << "):\n";
    if (devs.empty()) {
        std::cout << "  (none found — is an audio driver installed?)\n";
        return;
    }
    for (size_t i = 0; i < devs.size(); ++i) {
        std::cout << "  [" << i << "] " << devs[i].name;
        if (devs[i].isDefault) std::cout << "  <default>";
        std::cout << '\n';
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
#endif

    // ── Parse arguments ───────────────────────────────────────────────────────
    std::string deviceArg;
    bool        listOnly = false;

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
            std::cout <<
                "Usage: audio_monitor [--list-devices] [--device <name>]\n"
                "  --list-devices, -l     Print available input devices and exit.\n"
                "  --device <name>, -d    Capture from a device matching <name> "
                                         "(partial, case-insensitive).\n";
            return 0;
        }
    }

    // ── Enumerate devices ─────────────────────────────────────────────────────
    AudioDeviceManager mgr;
    const auto& devs = mgr.GetInputDevices();

    if (listOnly) {
        PrintDeviceList(devs);
        return 0;
    }

    if (devs.empty()) {
        std::cerr << "[Warning] No input devices found.\n";
        PrintDeviceList(devs);
        return 1;
    }

    // ── Start capture ─────────────────────────────────────────────────────────
    std::cout << "[EchoRadar Audio Monitor]\n";

    AudioCapture capture;
    bool started = false;

    if (!deviceArg.empty()) {
        const auto found = mgr.FindInputDeviceByName(deviceArg);
        if (found) {
            std::cout << "Using device: " << found->name << "\n\n";
        } else {
            std::cerr << "[Warning] Device '" << deviceArg
                      << "' not found. Falling back to default.\n\n";
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

    std::cout << "Press Ctrl+C to stop.\n\n";
    std::cout.flush();

    // ── Monitor loop ──────────────────────────────────────────────────────────
    constexpr int kInterval = 100; // ms
    int iterations = 0;

    std::cerr << "[debug] Entering monitor loop (g_running=" << g_running.load() << ")...\n" << std::flush;
    
    try {
        while (g_running.load(std::memory_order_relaxed)) {
            std::cerr << "[debug] Loop iteration " << (iterations + 1) << " starting, g_running=" << g_running.load() << "\n" << std::flush;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(kInterval));

            if (!capture.IsRunning()) {
                std::cerr << "[error] Capture stopped unexpectedly!\n" << std::flush;
                break;
            }

            const AudioLevels lvl      = capture.GetCurrentLevels();
            const size_t      buffered = capture.GetAvailableFrames();
            ++iterations;

            std::cout << std::fixed << std::setprecision(3)
                      << "[" << std::setw(4) << iterations << "] "
                      << "L RMS: " << std::setw(6) << lvl.leftRms
                      << "  R RMS: " << std::setw(6) << lvl.rightRms
                      << "  L Peak: " << std::setw(6) << lvl.leftPeak
                      << "  R Peak: " << std::setw(6) << lvl.rightPeak
                      << "  Buf: " << std::setw(6) << buffered << " fr"
                      << "  [" << Bar(lvl.leftRms) << "]"
                      << "\n";
            std::cout.flush();
            
            std::cerr << "[debug] Loop iteration " << iterations << " complete\n" << std::flush;
        }
    } catch (const std::exception& e) {
        std::cerr << "[error] Exception in monitor loop: " << e.what() << "\n" << std::flush;
    } catch (...) {
        std::cerr << "[error] Unknown exception in monitor loop\n" << std::flush;
    }
    
    std::cerr << "[debug] Exited monitor loop (g_running=" << g_running.load() << ", iterations=" << iterations << ")\n" << std::flush;

    std::cout << "\nStopping...\n";
    capture.Stop();
    return 0;
}
