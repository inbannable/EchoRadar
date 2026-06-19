#include "EchoRadarApp.h"
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    std::cout << "=== EchoRadar v0.1.0 ===\n"
              << "Real-Time Spatial Audio Detection System for FPS Games\n\n";

    EchoRadar::EchoRadarApp::Config cfg;
    cfg.show_overlay = true;

    EchoRadar::EchoRadarApp app(cfg);

    if (!app.Initialise()) {
        std::cerr << "[main] Initialisation failed. Exiting.\n";
        return 1;
    }

    app.Run();
    return 0;
}
