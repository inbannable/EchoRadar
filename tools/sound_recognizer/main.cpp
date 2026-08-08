#include <audio/AudioCapture.h>
#include <recognition/ModelPackage.h>
#include <recognition/OnnxProbabilityModel.h>
#include <recognition/SoundRecognizer.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace EchoRadar;

namespace {
std::atomic<bool> g_running{true};
void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
    std::filesystem::path modelDirectory;
    std::string device;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--model" && i + 1 < argc) modelDirectory = argv[++i];
        else if (argument == "--device" && i + 1 < argc) device = argv[++i];
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: sound_recognizer --model <package-dir> [--device <partial-name>]\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    if (modelDirectory.empty()) {
        std::cerr << "--model is required\n";
        return 2;
    }

    ModelPackage package;
    std::string error;
    if (!ModelPackage::Load(modelDirectory, package, &error)) {
        std::cerr << "Model package rejected: " << error << '\n';
        return 1;
    }
    auto model = std::make_shared<OnnxProbabilityModel>(package.modelPath,
                                                        package.contextFrames,
                                                        package.melBins,
                                                        package.inputChannels);
    if (!model->IsLoaded()) {
        std::cerr << "ONNX model could not be loaded: " << model->LoadError() << '\n';
        return 1;
    }
    SoundRecognizer::Config recognizerConfig;
    recognizerConfig.contextFrames = package.contextFrames;
    recognizerConfig.inferenceStrideFrames = package.inferenceStrideFrames;
    recognizerConfig.featureConfig.mode = package.preprocessingVersion == "stereo-pcen-v2"
        ? LogMelExtractor::Mode::StereoPcenV2 : LogMelExtractor::Mode::LogMelV1;
    recognizerConfig.featureConfig.pcenSmoothing = package.pcenSmoothing;
    recognizerConfig.featureConfig.pcenAlpha = package.pcenAlpha;
    recognizerConfig.featureConfig.pcenDelta = package.pcenDelta;
    recognizerConfig.featureConfig.pcenRoot = package.pcenRoot;
    recognizerConfig.featureConfig.pcenEpsilon = package.pcenEpsilon;
    recognizerConfig.postProcessing = package.postProcessing;
    SoundRecognizer recognizer(model, recognizerConfig);

    AudioCapture capture;
    const bool started = device.empty() ? capture.StartDefault() : capture.StartDeviceByName(device);
    if (!started) {
        std::cerr << "Could not start audio capture\n";
        return 1;
    }
    std::cout << "EchoRadar recognition monitor - Ctrl+C to stop\n"
              << "Model: " << package.modelVersion << '\n';
    constexpr size_t kChunkFrames = 480;
    std::vector<float> samples(kChunkFrames * 2);
    uint64_t eventCount = 0;
    while (g_running.load(std::memory_order_relaxed) && capture.IsRunning()) {
        const size_t frames = capture.ReadInterleaved(samples.data(), kChunkFrames);
        if (frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const auto events = recognizer.PushInterleaved(samples.data(), frames);
        for (const SoundEvent& event : events) {
            ++eventCount;
            std::printf("\n[%s] onset=%.3fs detected=%.3fs end=%.3fs confidence=%.3f\n",
                        ToString(event.soundClass), event.onsetSample / 48000.0,
                        event.detectedSample / 48000.0,
                        event.endSample / 48000.0, event.confidence);
        }
        const SoundProbabilities& p = recognizer.LastProbabilities();
        const SoundRecognitionState state = recognizer.CurrentState();
        const auto& stats = recognizer.Stats();
        std::string mode = state.IsAmbient() ? "AMBIENT" : "ACTIVE:";
        if (!state.IsAmbient()) {
            for (size_t index = 0; index < kSoundClassCount; ++index) {
                if (state.activeClasses[index]) {
                    mode += std::string(" ") + ToString(kSoundClasses[index]);
                }
            }
        }
        std::printf("mode %-32s | scores gun %.3f foot %.3f mech %.3f | infer %.2fms p95 %.2fms events %llu dropped %llu     \r",
                    mode.c_str(), p.values[0], p.values[1], p.values[2],
                    stats.lastInferenceMs, stats.p95InferenceMs,
                    static_cast<unsigned long long>(eventCount),
                    static_cast<unsigned long long>(capture.GetDroppedFrames()));
        std::fflush(stdout);
    }
    capture.Stop();
    std::cout << "\nStopped.\n";
    return 0;
}
