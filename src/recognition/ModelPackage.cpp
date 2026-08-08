#include "ModelPackage.h"

#include <dataset/AssetInventory.h>

#include <fstream>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>

namespace EchoRadar {
namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
    std::map<std::string, std::string> values;
    const std::regex pair(R"json("([^"\\]+)"\s*:\s*("(?:\\.|[^"])*"|[-+0-9.eE]+|true|false|null))json");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pair);
         it != std::sregex_iterator(); ++it) {
        std::string value = (*it)[2].str();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values[(*it)[1].str()] = value;
    }
    return values;
}

std::string Get(const std::map<std::string, std::string>& values, const std::string& key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string{} : it->second;
}

uint32_t GetU32(const std::map<std::string, std::string>& values,
                const std::string& key,
                uint32_t fallback) {
    try {
        const std::string value = Get(values, key);
        return value.empty() ? fallback : static_cast<uint32_t>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

float GetFloat(const std::map<std::string, std::string>& values,
               const std::string& key,
               float fallback) {
    try {
        const std::string value = Get(values, key);
        return value.empty() ? fallback : std::stof(value);
    } catch (...) {
        return fallback;
    }
}

} // namespace

bool ModelPackage::Load(const std::filesystem::path& directoryPath,
                        ModelPackage& package,
                        std::string* error) {
    package = {};
    const std::filesystem::path metadataPath = directoryPath / "model.json";
    std::ifstream in(metadataPath, std::ios::binary);
    if (!in) return Fail(error, "Missing model metadata: " + metadataPath.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto values = ParseFlatJson(buffer.str());

    package.directory = directoryPath;
    package.modelVersion = Get(values, "model_version");
    package.modelSha256 = Get(values, "model_sha256");
    package.preprocessingVersion = Get(values, "preprocessing_version");
    const std::string modelFile = Get(values, "model_file");
    if (package.modelVersion.empty() || package.modelSha256.empty() || modelFile.empty()) {
        return Fail(error, "Model metadata is missing version, file, or SHA-256");
    }
    const std::filesystem::path modelFilePath(modelFile);
    if (modelFilePath.is_absolute() || modelFilePath.has_parent_path()) {
        return Fail(error, "Model file must be a filename inside the package directory");
    }
    package.modelPath = directoryPath / modelFilePath;
    if (!std::filesystem::is_regular_file(package.modelPath)) {
        return Fail(error, "Model file is missing: " + package.modelPath.string());
    }

    package.sampleRate = GetU32(values, "sample_rate", 0);
    package.fftSize = GetU32(values, "fft_size", 0);
    package.hopSize = GetU32(values, "hop_size", 0);
    package.melBins = GetU32(values, "mel_bins", 0);
    package.contextFrames = GetU32(values, "context_frames", 0);
    package.inferenceStrideFrames = GetU32(values, "inference_stride_frames", 0);
    const bool legacy = package.preprocessingVersion == "logmel-v1";
    const bool onset = package.preprocessingVersion == "stereo-pcen-v2";
    package.inputChannels = GetU32(values, "input_channels", legacy ? 1u : 0u);
    if ((!legacy && !onset) || package.sampleRate != 48000 ||
        package.fftSize != 1024 || package.hopSize != 512 || package.melBins != 64 ||
        package.contextFrames != 96 || package.inferenceStrideFrames == 0 ||
        package.inputChannels != (onset ? 2u : 1u)) {
        return Fail(error, "Model preprocessing contract is incompatible with EchoRadar");
    }
    if (onset) {
        if (Get(values, "event_mode") != "onset-pulse") {
            return Fail(error, "stereo-pcen-v2 requires event_mode onset-pulse");
        }
        package.pcenSmoothing = GetFloat(values, "pcen_smoothing", 0.0f);
        package.pcenAlpha = GetFloat(values, "pcen_alpha", -1.0f);
        package.pcenDelta = GetFloat(values, "pcen_delta", 0.0f);
        package.pcenRoot = GetFloat(values, "pcen_root", 0.0f);
        package.pcenEpsilon = GetFloat(values, "pcen_epsilon", 0.0f);
        package.postProcessing.mode = EventPostProcessor::Mode::OnsetPulse;
        package.postProcessing.pulseSamples =
            static_cast<uint64_t>(GetU32(values, "pulse_ms", 0)) * package.sampleRate / 1000u;
        if (package.pcenSmoothing <= 0.0f || package.pcenSmoothing > 1.0f ||
            package.pcenAlpha < 0.0f || package.pcenAlpha > 1.0f || package.pcenDelta <= 0.0f ||
            package.pcenRoot <= 0.0f || package.pcenRoot > 1.0f || package.pcenEpsilon <= 0.0f ||
            package.postProcessing.pulseSamples == 0) {
            return Fail(error, "Model PCEN or onset-pulse metadata is invalid");
        }
    }
    if (Get(values, "class_order") != "gunshot,footstep,mechanical") {
        return Fail(error, "Model class order must be gunshot,footstep,mechanical");
    }

    package.postProcessing.modelVersion = package.modelVersion;
    const std::array<std::string, kSoundClassCount> names{"gunshot", "footstep", "mechanical"};
    for (size_t i = 0; i < names.size(); ++i) {
        const std::array<std::string, 5> requiredKeys{
            "threshold_" + names[i], "off_threshold_" + names[i],
            "min_on_frames_" + names[i], "min_off_frames_" + names[i],
            "refractory_ms_" + names[i],
        };
        for (const std::string& key : requiredKeys) {
            if (Get(values, key).empty()) return Fail(error, "Model metadata is missing " + key);
        }
        package.postProcessing.onThresholds[i] = GetFloat(values, "threshold_" + names[i], 0.5f);
        package.postProcessing.offThresholds[i] = GetFloat(values, "off_threshold_" + names[i],
                                                            package.postProcessing.onThresholds[i] * 0.7f);
        package.postProcessing.rearmThresholds[i] = onset
            ? GetFloat(values, "rearm_threshold_" + names[i], -1.0f)
            : package.postProcessing.offThresholds[i];
        package.postProcessing.minOnFrames[i] = GetU32(values, "min_on_frames_" + names[i], 1);
        package.postProcessing.minOffFrames[i] = GetU32(values, "min_off_frames_" + names[i], 2);
        const uint32_t refractoryMs = GetU32(values, "refractory_ms_" + names[i], i == 2 ? 100 : 50);
        package.postProcessing.refractorySamples[i] =
            static_cast<uint64_t>(refractoryMs) * package.sampleRate / 1000u;
        package.postProcessing.onsetOffsetSamples[i] = onset
            ? GetU32(values, "onset_offset_samples_" + names[i], UINT32_MAX) : 0u;
        if (!std::isfinite(package.postProcessing.onThresholds[i]) ||
            !std::isfinite(package.postProcessing.offThresholds[i]) ||
            !std::isfinite(package.postProcessing.rearmThresholds[i]) ||
            package.postProcessing.onThresholds[i] <= 0.0f ||
            package.postProcessing.onThresholds[i] > 1.0f ||
            package.postProcessing.offThresholds[i] < 0.0f ||
            package.postProcessing.offThresholds[i] >= package.postProcessing.onThresholds[i] ||
            package.postProcessing.rearmThresholds[i] < 0.0f ||
            package.postProcessing.rearmThresholds[i] >= package.postProcessing.onThresholds[i] ||
            (onset && package.postProcessing.onsetOffsetSamples[i] == UINT32_MAX) ||
            package.postProcessing.minOnFrames[i] == 0 || package.postProcessing.minOffFrames[i] == 0) {
            return Fail(error, "Model post-processing contract is invalid for " + names[i]);
        }
    }

    bool hashOk = false;
    const std::string actualSha256 = AssetInventory::ComputeFileSha256(package.modelPath, &hashOk);
    if (!hashOk || actualSha256 != package.modelSha256) {
        return Fail(error, "Model SHA-256 does not match model.json");
    }
    if (error) error->clear();
    return true;
}

} // namespace EchoRadar
