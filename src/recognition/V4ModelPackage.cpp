#include "V4ModelPackage.h"

#include <dataset/AssetInventory.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
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
    const std::regex pair(
        R"json("([^"\\]+)"\s*:\s*("(?:\\.|[^"])*"|[-+0-9.eE]+|true|false|null))json");
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
    const auto found = values.find(key);
    return found == values.end() ? std::string{} : found->second;
}

bool ReadU32(const std::map<std::string, std::string>& values, const std::string& key,
             uint32_t& output) {
    try {
        const std::string text = Get(values, key);
        if (text.empty() || text.front() == '-') return false;
        size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size() || value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        output = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadFloat(const std::map<std::string, std::string>& values, const std::string& key,
               float& output) {
    try {
        const std::string text = Get(values, key);
        if (text.empty()) return false;
        output = std::stof(text);
        return std::isfinite(output);
    } catch (...) {
        return false;
    }
}

bool Close(float left, float right) {
    return std::abs(left - right) <= 1e-7f * std::max(1.0f, std::abs(right));
}

} // namespace

bool V4ModelPackage::Load(const std::filesystem::path& directoryPath,
                          V4ModelPackage& package,
                          std::string* error) {
    package = {};
    const std::filesystem::path metadataPath = directoryPath / "model.json";
    std::ifstream input(metadataPath, std::ios::binary);
    if (!input) return Fail(error, "Missing V4 model metadata: " + metadataPath.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto values = ParseFlatJson(buffer.str());

    uint32_t packageVersion = 0;
    if (!ReadU32(values, "package_version", packageVersion) || packageVersion != 4) {
        return Fail(error, "V4 model package_version must be 4");
    }
    package.directory = directoryPath;
    package.modelVersion = Get(values, "model_version");
    package.modelSha256 = Get(values, "model_sha256");
    package.preprocessingVersion = Get(values, "preprocessing_version");
    const std::string modelFile = Get(values, "model_file");
    if (package.modelVersion.empty() || package.modelSha256.empty() || modelFile.empty()) {
        return Fail(error, "V4 model metadata is missing version, file, or SHA-256");
    }
    const std::filesystem::path modelFilename(modelFile);
    if (modelFilename.is_absolute() || modelFilename.has_parent_path()) {
        return Fail(error, "V4 model file must be inside its package directory");
    }
    package.modelPath = directoryPath / modelFilename;
    if (!std::filesystem::is_regular_file(package.modelPath)) {
        return Fail(error, "V4 ONNX model is missing: " + package.modelPath.string());
    }

    if (!ReadU32(values, "sample_rate", package.sampleRate) ||
        !ReadU32(values, "fft_size", package.fftSize) ||
        !ReadU32(values, "hop_size", package.hopSize) ||
        !ReadU32(values, "mel_bins", package.melBins) ||
        !ReadU32(values, "context_frames", package.contextFrames) ||
        !ReadU32(values, "input_channels", package.inputChannels) ||
        !ReadU32(values, "inference_stride_frames", package.inferenceStrideFrames) ||
        package.preprocessingVersion != "stereo-onset-v4" || package.sampleRate != 48000 ||
        package.fftSize != 1024 || package.hopSize != 240 || package.melBins != 64 ||
        package.contextFrames != 128 || package.inputChannels != 5 ||
        package.inferenceStrideFrames != 2 ||
        Get(values, "class_order") != "gunshot,footstep" ||
        Get(values, "source_order") != "self,remote,unknown" ||
        Get(values, "event_mode") != "onset-peak") {
        return Fail(error, "V4 preprocessing, class, source, or event contract is incompatible");
    }

    if (!ReadFloat(values, "pcen_smoothing", package.pcenSmoothing) ||
        !ReadFloat(values, "pcen_alpha", package.pcenAlpha) ||
        !ReadFloat(values, "pcen_delta", package.pcenDelta) ||
        !ReadFloat(values, "pcen_root", package.pcenRoot) ||
        !ReadFloat(values, "pcen_epsilon", package.pcenEpsilon) ||
        !Close(package.pcenSmoothing, 0.025f) || !Close(package.pcenAlpha, 0.98f) ||
        !Close(package.pcenDelta, 2.0f) || !Close(package.pcenRoot, 0.5f) ||
        !Close(package.pcenEpsilon, 1e-6f)) {
        return Fail(error, "V4 PCEN metadata is incompatible");
    }
    if (!ReadFloat(values, "scene_activity_cutoff", package.sceneActivityCutoff) ||
        !ReadFloat(values, "self_suppression_threshold", package.selfSuppressionThreshold) ||
        !ReadU32(values, "peak_lookahead_frames", package.peakLookaheadFrames)) {
        return Fail(error, "V4 scene/source/peak policy is missing");
    }
    uint32_t pulseMs = 0;
    if (!ReadU32(values, "pulse_ms", pulseMs) || pulseMs == 0 ||
        package.sceneActivityCutoff <= 0.0f || package.sceneActivityCutoff >= 1.0f ||
        package.selfSuppressionThreshold <= 0.0f || package.selfSuppressionThreshold > 1.0f) {
        return Fail(error, "V4 scene/source/peak policy is invalid");
    }
    package.pulseSamples = static_cast<uint64_t>(pulseMs) * package.sampleRate / 1000u;

    const std::array<std::string, 2> names{"gunshot", "footstep"};
    for (size_t index = 0; index < names.size(); ++index) {
        uint32_t spacingMs = 0;
        if (!ReadFloat(values, "threshold_quiet_" + names[index], package.quietThresholds[index]) ||
            !ReadFloat(values, "threshold_busy_" + names[index], package.busyThresholds[index]) ||
            !ReadU32(values, "minimum_spacing_ms_" + names[index], spacingMs) ||
            !ReadU32(values, "onset_offset_samples_" + names[index],
                     package.onsetOffsetSamples[index]) ||
            package.quietThresholds[index] <= 0.0f || package.quietThresholds[index] > 1.0f ||
            package.busyThresholds[index] <= 0.0f || package.busyThresholds[index] > 1.0f ||
            spacingMs == 0) {
            return Fail(error, "V4 calibrated policy is invalid for " + names[index]);
        }
        package.minimumSpacingSamples[index] =
            static_cast<uint64_t>(spacingMs) * package.sampleRate / 1000u;
    }

    bool hashOk = false;
    const std::string actualHash = AssetInventory::ComputeFileSha256(package.modelPath, &hashOk);
    if (!hashOk || actualHash != package.modelSha256) {
        return Fail(error, "V4 model SHA-256 does not match model.json");
    }
    if (error) error->clear();
    return true;
}

} // namespace EchoRadar
