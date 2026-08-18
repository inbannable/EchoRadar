#include "EchoRadarApp.h"

#include <audio/AudioDeviceManager.h>

#include <csignal>
#include <filesystem>
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
    if (g_app != nullptr) g_app->Stop();
}

std::filesystem::path ExecutablePath(const char* argumentZero) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size()) {
        return std::filesystem::path(std::wstring(buffer.data(), length));
    }
#endif
    std::error_code error;
    const auto absolute = std::filesystem::absolute(argumentZero, error);
    return error ? std::filesystem::path(argumentZero) : absolute;
}

bool IsV4ModelPackage(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / "model.json", error);
}

std::filesystem::path ResolveDefaultModelDirectory(const char* argumentZero) {
    const auto executableDirectory = ExecutablePath(argumentZero).parent_path();
    std::vector<std::filesystem::path> candidates{
        executableDirectory / "models" / "v4-candidate",
    };

    std::error_code error;
    const auto workingDirectory = std::filesystem::current_path(error);
    if (!error) candidates.push_back(workingDirectory / "models" / "v4-candidate");

    // A development build normally lives at build/src/app/<config>. Walk back
    // to the checkout so launching the executable from any directory still
    // finds the locally trained package.
    for (auto ancestor = executableDirectory; !ancestor.empty();) {
        candidates.push_back(ancestor / "models" / "v4-candidate");
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }

    for (const auto& candidate : candidates) {
        if (!IsV4ModelPackage(candidate)) continue;
        const auto normalized = std::filesystem::weakly_canonical(candidate, error);
        return error ? candidate : normalized;
    }
    return workingDirectory.empty()
        ? std::filesystem::path("models/v4-candidate")
        : workingDirectory / "models" / "v4-candidate";
}

void PrintOutputs() {
    EchoRadar::AudioDeviceManager manager;
    const auto& outputs = manager.GetOutputDevices();
    std::cout << "Output endpoints (" << outputs.size() << "):\n";
    for (const auto& output : outputs) {
        std::cout << "  " << output.id << "  " << output.name;
        if (output.isDefault) std::cout << "  <default>";
        if (output.nativeChannels != 0) {
            std::cout << "  " << output.nativeChannels << "ch@" << output.nativeSampleRate;
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // miniaudio exposes endpoint names as UTF-8. Match the console code page so
    // localized device names are not printed as mojibake.
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    EchoRadar::EchoRadarApp::Config config;
    bool listOutputs = false;
    bool modelWasExplicit = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--list-audio-outputs") {
            listOutputs = true;
        } else if (argument == "--audio-output-id" && index + 1 < argc) {
            config.audio.selection = EchoRadar::AudioEndpointSelection::Fixed;
            config.audio.endpointId = argv[++index];
        } else if (argument == "--model" && index + 1 < argc) {
            config.modelDirectory = argv[++index];
            modelWasExplicit = true;
        } else if (argument == "--direction-model" && index + 1 < argc) {
            config.directionModelDirectory = argv[++index];
        } else if (argument == "--legacy-direction-diagnostic") {
            config.legacyDirectionDiagnostic = true;
        } else if (argument == "--settings" && index + 1 < argc) {
            config.settingsPath = argv[++index];
        } else if (argument == "--no-overlay") {
            config.show_overlay = false;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: EchoRadar [options]\n\n"
                << "  --list-audio-outputs       List render endpoints for loopback\n"
                << "  --audio-output-id <id>     Pin capture to one render endpoint\n"
                << "  --model <package-dir>      Load an experimental V4 package\n"
                << "  --direction-model <dir>    Load the scene-level 3D direction package\n"
                << "  --legacy-direction-diagnostic\n"
                << "                              Explicitly run the old single-source mapper\n"
                << "  --settings <json>          Override the per-user settings path\n"
                << "  --no-overlay               Do not initialize the V4 event chart UI\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << argument << '\n';
            return 2;
        }
    }
    if (listOutputs) {
        PrintOutputs();
        return 0;
    }
    if (!config.directionModelDirectory.empty() && config.legacyDirectionDiagnostic) {
        std::cerr << "--direction-model and --legacy-direction-diagnostic are mutually exclusive\n";
        return 2;
    }

    if (!modelWasExplicit) config.modelDirectory = ResolveDefaultModelDirectory(argv[0]);

    std::cout << "=== EchoRadar experimental V4 ===\n";
    std::cout << "[EchoRadar] V4 model package: " << config.modelDirectory.string() << '\n';
    if (!config.directionModelDirectory.empty()) {
        std::cout << "[EchoRadar] Direction model package: "
                  << config.directionModelDirectory.string() << '\n';
    } else if (config.legacyDirectionDiagnostic) {
        std::cout << "[EchoRadar] Legacy single-source direction diagnostic enabled\n";
    } else {
        std::cout << "[EchoRadar] Direction inference disabled (use --direction-model)\n";
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
