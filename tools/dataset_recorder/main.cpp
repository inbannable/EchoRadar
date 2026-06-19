/// tools/dataset_recorder/main.cpp
/// ─────────────────────────────────────────────────────────────────────────────
/// Dataset recorder tool.
/// Press SPACE to start/stop recording; ESC to quit.
/// Saves WAV files to datasets/<angle>deg.wav
///
/// Full implementation: Milestone 6.
/// ─────────────────────────────────────────────────────────────────────────────

#include <audio/AudioCapture.h>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int angle_deg = 0;
    if (argc > 1) angle_deg = std::stoi(argv[1]);

    std::cout << "=== EchoRadar Dataset Recorder ===\n"
              << "Angle: " << angle_deg << " degrees\n"
              << "Output: datasets/" << angle_deg << "deg.wav\n\n"
              << "Press ENTER to record 5 seconds, Ctrl-C to quit.\n";

    // TODO (Milestone 6): initialise AudioCapture, capture frames to WAV file.
    std::cin.get();

    std::cout << "[dataset_recorder] Recording... (stub, not yet implemented)\n"
              << "[dataset_recorder] Done.\n";
    return 0;
}
