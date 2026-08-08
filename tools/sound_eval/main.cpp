#include <recognition/Evaluation.h>
#include <recognition/ModelPackage.h>
#include <recognition/OnnxProbabilityModel.h>
#include <recognition/PcmWav.h>
#include <recognition/SoundRecognizer.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

using namespace EchoRadar;

int main(int argc, char* argv[]) {
    std::filesystem::path modelDirectory;
    std::filesystem::path wavPath;
    std::filesystem::path timelinePath;
    std::filesystem::path reportPath;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--model" && i + 1 < argc) modelDirectory = argv[++i];
        else if (argument == "--wav" && i + 1 < argc) wavPath = argv[++i];
        else if (argument == "--timeline" && i + 1 < argc) timelinePath = argv[++i];
        else if (argument == "--report" && i + 1 < argc) reportPath = argv[++i];
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: sound_eval --model <dir> --wav <file> --timeline <jsonl> --report <json>\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    if (modelDirectory.empty() || wavPath.empty() || timelinePath.empty() || reportPath.empty()) {
        std::cerr << "--model, --wav, --timeline, and --report are required\n";
        return 2;
    }

    std::string error;
    ModelPackage package;
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
    PcmAudio source;
    if (!LoadPcmWav(wavPath, source, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    PcmAudio audio = ConvertToStereo48k(source);
    std::vector<LabeledSoundEvent> truth;
    if (!LoadTimelineJsonl(timelinePath, truth, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    SoundRecognizer::Config config;
    config.contextFrames = package.contextFrames;
    config.inferenceStrideFrames = package.inferenceStrideFrames;
    config.featureConfig.mode = package.preprocessingVersion == "stereo-pcen-v2"
        ? LogMelExtractor::Mode::StereoPcenV2 : LogMelExtractor::Mode::LogMelV1;
    config.featureConfig.pcenSmoothing = package.pcenSmoothing;
    config.featureConfig.pcenAlpha = package.pcenAlpha;
    config.featureConfig.pcenDelta = package.pcenDelta;
    config.featureConfig.pcenRoot = package.pcenRoot;
    config.featureConfig.pcenEpsilon = package.pcenEpsilon;
    config.postProcessing = package.postProcessing;
    SoundRecognizer recognizer(model, config);
    std::vector<SoundEvent> predictions;
    constexpr size_t kChunkFrames = 480;
    for (size_t frame = 0; frame < audio.FrameCount(); frame += kChunkFrames) {
        const size_t count = std::min(kChunkFrames, audio.FrameCount() - frame);
        auto events = recognizer.PushInterleaved(audio.interleaved.data() + frame * 2, count);
        predictions.insert(predictions.end(), events.begin(), events.end());
    }
    auto finalEvents = recognizer.Flush();
    predictions.insert(predictions.end(), finalEvents.begin(), finalEvents.end());
    const EvaluationReport report = EvaluateEvents(
        truth, predictions, static_cast<double>(audio.FrameCount()) / audio.sampleRate);
    if (!WriteEvaluationJson(reportPath, report, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::filesystem::path predictionsPath = reportPath.parent_path() /
        (reportPath.stem().string() + ".predictions.jsonl");
    std::ofstream predictionOutput(predictionsPath);
    for (const SoundEvent& event : predictions) {
        predictionOutput << "{\"class\":\"" << ToString(event.soundClass)
                         << "\",\"onset_sample\":" << event.onsetSample
                         << ",\"end_sample\":" << event.endSample
                         << ",\"detected_sample\":" << event.detectedSample
                         << ",\"confidence\":" << std::setprecision(7) << event.confidence
                         << ",\"model_version\":\"" << event.modelVersion << "\"}\n";
    }

    for (size_t i = 0; i < kSoundClassCount; ++i) {
        const ClassMetrics& metrics = report.classes[i];
        std::cout << std::setw(10) << ToString(kSoundClasses[i])
                  << " precision=" << std::fixed << std::setprecision(3) << metrics.precision
                  << " recall=" << metrics.recall << " f1=" << metrics.f1
                  << " fp/min=" << metrics.falseAlertsPerMinute << '\n';
    }
    const auto& runtime = recognizer.Stats();
    std::cout << "Runtime: " << runtime.inferenceCount << " inferences, last="
              << std::fixed << std::setprecision(3) << runtime.lastInferenceMs
              << " ms, p95=" << runtime.p95InferenceMs
              << " ms, max=" << runtime.maxInferenceMs << " ms\n";
    std::cout << "Report: " << reportPath << '\n';
    return 0;
}
