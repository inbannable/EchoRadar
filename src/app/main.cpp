#include "EchoRadarApp.h"

#include <audio/AudioDeviceManager.h>
#include <recognition/RecognitionModelLocator.h>

#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

EchoRadar::EchoRadarApp* g_app = nullptr;

void OnSignal(int) {
    if (g_app) g_app->Stop();
}

std::filesystem::path ExecutablePath(const char* argumentZero) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size()) {
        return std::filesystem::path(
            std::wstring(buffer.data(), length));
    }
#endif
    std::error_code error;
    const auto absolute =
        std::filesystem::absolute(argumentZero, error);
    return error ? std::filesystem::path(argumentZero) : absolute;
}

std::filesystem::path ResolveDefaultModelDirectory(
    const char* argumentZero) {
    std::error_code error;
    const auto workingDirectory =
        std::filesystem::current_path(error);
    return EchoRadar::FindDefaultRecognitionModelDirectory(
        ExecutablePath(argumentZero), error ? std::filesystem::path{}
                                            : workingDirectory);
}

void PrintOutputs() {
    EchoRadar::AudioDeviceManager manager;
    const auto& outputs = manager.GetOutputDevices();
    std::cout << "Output endpoints (" << outputs.size() << "):\n";
    for (const auto& output : outputs) {
        std::cout << "  " << output.id << "  " << output.name;
        if (output.isDefault) std::cout << "  <default>";
        if (output.nativeChannels != 0) {
            std::cout << "  " << output.nativeChannels
                      << "ch@" << output.nativeSampleRate
                      << "  " << EchoRadar::ToString(output.layout.kind)
                      << "  mask=0x" << std::hex << output.nativeChannelMask
                      << std::dec << "  [";
            for (uint32_t index = 0; index < output.layout.channelCount; ++index) {
                if (index != 0) std::cout << ',';
                std::cout << EchoRadar::ToString(output.layout.roles[index]);
            }
            std::cout << ']';
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    EchoRadar::EchoRadarApp::Config config;
    bool listOutputs = false;
    bool modelWasExplicit = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--list-audio-outputs") {
            listOutputs = true;
        } else if (argument == "--audio-output-id" &&
                   index + 1 < argc) {
            config.audio.selection =
                EchoRadar::AudioEndpointSelection::Fixed;
            config.audio.endpointId = argv[++index];
        } else if (argument == "--model" && index + 1 < argc) {
            config.modelDirectory = argv[++index];
            modelWasExplicit = true;
        } else if (argument == "--radar-mode" && index + 1 < argc) {
            const std::string value(argv[++index]);
            if (value == "continuous") {
                config.radarMode = EchoRadar::RadarMode::Continuous;
            } else if (value == "events") {
                config.radarMode = EchoRadar::RadarMode::Events;
            } else if (value == "combined") {
                config.radarMode = EchoRadar::RadarMode::Combined;
            } else {
                std::cerr << "--radar-mode must be continuous, events, or combined\n";
                return 2;
            }
        } else if (argument == "--radar-preset" && index + 1 < argc) {
            const std::string value(argv[++index]);
            if (value == "all") {
                config.radarPreset = EchoRadar::RadarPreset::All;
            } else if (value == "footsteps") {
                config.radarPreset = EchoRadar::RadarPreset::Footsteps;
            } else if (value == "gunshots") {
                config.radarPreset = EchoRadar::RadarPreset::Gunshots;
            } else if (value == "custom") {
                config.radarPreset = EchoRadar::RadarPreset::Custom;
            } else {
                std::cerr << "--radar-preset must be all, footsteps, gunshots, or custom\n";
                return 2;
            }
        } else if (argument == "--settings" && index + 1 < argc) {
            config.settingsPath = argv[++index];
        } else if (argument == "--no-overlay") {
            config.showOverlay = false;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: EchoRadarV2 [options]\n\n"
                << "  --list-audio-outputs       List render endpoints for loopback\n"
                << "  --audio-output-id <id>     Pin capture to one render endpoint\n"
                << "  --radar-mode <mode>        continuous, events, or combined\n"
                << "  --radar-preset <preset>    all, footsteps, gunshots, or custom\n"
                << "  --model <package-dir>      Optional recognition trigger package\n"
                << "  --settings <json>          Override the per-user settings path\n"
                << "  --no-overlay               Run without the control UI or HUD\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: "
                      << argument << '\n';
            return 2;
        }
    }

    if (listOutputs) {
        PrintOutputs();
        return 0;
    }
    if (!modelWasExplicit) {
        config.modelDirectory =
            ResolveDefaultModelDirectory(argv[0]);
    }

    std::cout << "=== EchoRadar v2 multichannel research build ===\n";
    if (!config.modelDirectory.empty()) {
        std::cout << "[EchoRadar] Recognition package: "
                  << config.modelDirectory.string() << '\n';
    } else {
        std::cout << "[EchoRadar] Recognition disabled; continuous radar needs no model\n";
    }

    EchoRadar::EchoRadarApp app(config);
    g_app = &app;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    if (!app.Initialise()) {
        g_app = nullptr;
        return 1;
    }
    app.Run();
    g_app = nullptr;
    return 0;
}
