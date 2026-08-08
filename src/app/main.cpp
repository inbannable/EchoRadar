#include "EchoRadarApp.h"

#include <audio/AudioDeviceManager.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

EchoRadar::EchoRadarApp* g_app = nullptr;

void OnSignal(int) {
    if (g_app != nullptr) g_app->Stop();
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
        } else if (argument == "--no-overlay") {
            config.show_overlay = false;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: EchoRadar [options]\n\n"
                << "  --list-audio-outputs       List render endpoints for loopback\n"
                << "  --audio-output-id <id>     Pin capture to one render endpoint\n"
                << "  --model <package-dir>      Load an experimental V4 package\n"
                << "  --no-overlay               Do not initialize the overlay stub\n";
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

    if (!modelWasExplicit) {
        const std::filesystem::path besideExecutable =
            std::filesystem::absolute(argv[0]).parent_path() / "models" / "v4-candidate";
        if (std::filesystem::is_directory(besideExecutable)) {
            config.modelDirectory = besideExecutable;
        }
    }

    std::cout << "=== EchoRadar experimental V4 ===\n";
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
