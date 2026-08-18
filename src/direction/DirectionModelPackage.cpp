#include "DirectionModelPackage.h"

#include <support/FileSha256.h>
#include <support/FlatJson.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace EchoRadar {
namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::string Get(const std::map<std::string, std::string>& values,
                const std::string& key) {
    const auto found = values.find(key);
    return found == values.end() ? std::string{} : found->second;
}

bool ReadU32(const std::map<std::string, std::string>& values,
             const std::string& key, uint32_t& output) {
    try {
        const std::string text = Get(values, key);
        if (text.empty() || text.front() == '-') return false;
        size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed);
        if (consumed != text.size() || parsed > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        output = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadFloat(const std::map<std::string, std::string>& values,
               const std::string& key, float& output) {
    try {
        const std::string text = Get(values, key);
        if (text.empty()) return false;
        size_t consumed = 0;
        output = std::stof(text, &consumed);
        return consumed == text.size() && std::isfinite(output);
    } catch (...) {
        return false;
    }
}

bool Close(float left, float right) {
    return std::abs(left - right) <= 1.0e-7f * std::max(1.0f, std::abs(right));
}

} // namespace

float DirectionModelPackage::UncertaintyDegrees(float confidence) const {
    if (uncertaintyCount == 0) return 180.0f;
    confidence = std::clamp(confidence, 0.0f, 1.0f);
    if (confidence <= uncertainty[0].confidence) {
        return uncertainty[0].p90AngularErrorDegrees;
    }
    for (uint32_t index = 1; index < uncertaintyCount; ++index) {
        if (confidence > uncertainty[index].confidence) continue;
        const auto& left = uncertainty[index - 1];
        const auto& right = uncertainty[index];
        const float width = right.confidence - left.confidence;
        const float ratio = width > 1.0e-6f
            ? (confidence - left.confidence) / width : 0.0f;
        return std::lerp(left.p90AngularErrorDegrees,
                         right.p90AngularErrorDegrees, ratio);
    }
    return uncertainty[uncertaintyCount - 1].p90AngularErrorDegrees;
}

bool DirectionModelPackage::Load(const std::filesystem::path& directoryPath,
                                 DirectionModelPackage& package,
                                 std::string* error) {
    package = {};
    const auto metadataPath = directoryPath / "direction.json";
    std::ifstream input(metadataPath, std::ios::binary);
    if (!input) return Fail(error, "Missing direction model metadata: " + metadataPath.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto values = detail::ParseFlatJson(buffer.str());

    uint32_t packageVersion = 0;
    uint32_t trackCount = 0;
    if (!ReadU32(values, "package_version", packageVersion) || packageVersion != 1 ||
        !ReadU32(values, "track_count", trackCount) || trackCount != kTrackCount) {
        return Fail(error, "Direction package version or track count is incompatible");
    }
    package.directory = directoryPath;
    package.modelVersion = Get(values, "model_version");
    package.modelSha256 = Get(values, "model_sha256");
    package.preprocessingVersion = Get(values, "preprocessing_version");
    const std::filesystem::path modelFilename(Get(values, "model_file"));
    if (package.modelVersion.empty() || package.modelSha256.empty() ||
        modelFilename.empty() || modelFilename.is_absolute() || modelFilename.has_parent_path()) {
        return Fail(error, "Direction metadata has an invalid model version, file, or SHA-256");
    }
    package.modelPath = directoryPath / modelFilename;
    if (!std::filesystem::is_regular_file(package.modelPath)) {
        return Fail(error, "Direction ONNX model is missing: " + package.modelPath.string());
    }

    if (!ReadU32(values, "sample_rate", package.sampleRate) || package.sampleRate != 48000 ||
        !ReadU32(values, "fft_size", package.fftSize) || package.fftSize != 1024 ||
        !ReadU32(values, "hop_size", package.hopSize) || package.hopSize != 240 ||
        !ReadU32(values, "mel_bins", package.melBins) || package.melBins != 64 ||
        !ReadU32(values, "context_frames", package.contextFrames) || package.contextFrames != 48 ||
        !ReadU32(values, "context_samples", package.contextSamples) || package.contextSamples != 12304 ||
        !ReadU32(values, "input_channels", package.inputChannels) || package.inputChannels != 5 ||
        !ReadU32(values, "maximum_sources", package.maximumSources) || package.maximumSources != 3 ||
        package.preprocessingVersion != "stereo-onset-v4-scene48" ||
        Get(values, "class_order") != "gunshot,footstep" ||
        Get(values, "track_order") != "exchangeable-0,exchangeable-1,exchangeable-2" ||
        Get(values, "coordinate_system") != "x-right,y-up,z-forward") {
        return Fail(error, "Direction preprocessing, class, or coordinate contract is incompatible");
    }
    if (package.contextSamples != package.fftSize +
            (package.contextFrames - 1u) * package.hopSize) {
        return Fail(error, "Direction context sample/frame contract is inconsistent");
    }

    if (!ReadFloat(values, "elevation_min_degrees", package.elevationMinDegrees) ||
        !ReadFloat(values, "elevation_max_degrees", package.elevationMaxDegrees) ||
        package.elevationMinDegrees != -60.0f || package.elevationMaxDegrees != 60.0f ||
        !ReadFloat(values, "threshold_gunshot", package.activityThresholds[0]) ||
        !ReadFloat(values, "threshold_footstep", package.activityThresholds[1]) ||
        !ReadFloat(values, "duplicate_merge_degrees", package.duplicateMergeDegrees) ||
        !ReadFloat(values, "minimum_training_separation_degrees",
                   package.minimumTrainingSeparationDegrees) ||
        package.activityThresholds[0] <= 0.0f || package.activityThresholds[0] > 1.0f ||
        package.activityThresholds[1] <= 0.0f || package.activityThresholds[1] > 1.0f ||
        package.duplicateMergeDegrees <= 0.0f || package.duplicateMergeDegrees >= 15.0f ||
        package.minimumTrainingSeparationDegrees != 15.0f) {
        return Fail(error, "Direction activity, elevation, or duplicate policy is invalid");
    }
    if (!ReadFloat(values, "pcen_smoothing", package.pcenSmoothing) ||
        !ReadFloat(values, "pcen_alpha", package.pcenAlpha) ||
        !ReadFloat(values, "pcen_delta", package.pcenDelta) ||
        !ReadFloat(values, "pcen_root", package.pcenRoot) ||
        !ReadFloat(values, "pcen_epsilon", package.pcenEpsilon) ||
        !Close(package.pcenSmoothing, 0.025f) || !Close(package.pcenAlpha, 0.98f) ||
        !Close(package.pcenDelta, 2.0f) || !Close(package.pcenRoot, 0.5f) ||
        !Close(package.pcenEpsilon, 1.0e-6f)) {
        return Fail(error, "Direction PCEN metadata is incompatible");
    }

    if (!ReadU32(values, "uncertainty_count", package.uncertaintyCount) ||
        package.uncertaintyCount < 2 ||
        package.uncertaintyCount > kMaximumUncertaintyPoints) {
        return Fail(error, "Direction uncertainty calibration count is invalid");
    }
    float previousConfidence = -1.0f;
    float previousUncertainty = 181.0f;
    for (uint32_t index = 0; index < package.uncertaintyCount; ++index) {
        auto& point = package.uncertainty[index];
        if (!ReadFloat(values, "uncertainty_confidence_" + std::to_string(index),
                       point.confidence) ||
            !ReadFloat(values, "uncertainty_p90_degrees_" + std::to_string(index),
                       point.p90AngularErrorDegrees) ||
            point.confidence < 0.0f || point.confidence > 1.0f ||
            point.confidence <= previousConfidence ||
            point.p90AngularErrorDegrees < 0.0f || point.p90AngularErrorDegrees > 180.0f ||
            point.p90AngularErrorDegrees > previousUncertainty + 1.0e-4f) {
            return Fail(error, "Direction uncertainty calibration is invalid or non-monotonic");
        }
        previousConfidence = point.confidence;
        previousUncertainty = point.p90AngularErrorDegrees;
    }

    bool hashOk = false;
    const std::string actualHash = ComputeFileSha256(package.modelPath, &hashOk);
    if (!hashOk || actualHash != package.modelSha256) {
        return Fail(error, "Direction model SHA-256 does not match direction.json");
    }
    if (error) error->clear();
    return true;
}

} // namespace EchoRadar
