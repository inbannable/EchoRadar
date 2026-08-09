#pragma once

#include "LocalizationTypes.h"

#include <recognition/SoundRecognitionTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace EchoRadar {

struct StereoDirectionFeatures {
    static constexpr size_t kBandCount = 8;

    float broadbandIldDb{0.0f};
    float itdSamples{0.0f};
    float correlationPeak{0.0f};
    float stereoQuality{0.0f};
    float rms{0.0f};
    std::array<float, kBandCount> bandIldDb{};
    std::array<float, kBandCount> bandCoherence{};
};

struct DirectionCalibrationSample {
    SoundClass soundClass{SoundClass::Footstep};
    float angleDegrees{0.0f};
    StereoDirectionFeatures features;
};

class DirectionCalibrationProfile {
public:
    void Clear();
    void SetAudioProfileKey(std::string key);
    const std::string& AudioProfileKey() const { return m_audioProfileKey; }
    bool Matches(const AudioProfile& profile) const;

    void AddSample(const DirectionCalibrationSample& sample);
    size_t SampleCount() const { return m_samples.size(); }
    size_t SampleCount(SoundClass soundClass) const;
    const std::vector<DirectionCalibrationSample>& Samples() const { return m_samples; }

    bool Save(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool Load(const std::filesystem::path& path, std::string* error = nullptr);

private:
    std::string m_audioProfileKey;
    std::vector<DirectionCalibrationSample> m_samples;
};

/// Estimates one independent direction distribution from one recognized event.
/// The built-in path provides an immediately usable stereo baseline and blends
/// in known-angle user calibration prototypes when available.
class StereoDirectionEstimator {
public:
    struct Config {
        uint32_t sampleRate{48000};
        uint32_t fftSize{1024};
        uint32_t hopSize{240};
        uint32_t maximumLagSamples{32};
    };

    StereoDirectionEstimator();
    explicit StereoDirectionEstimator(Config config);

    bool ExtractFeatures(std::span<const float> interleaved,
                         StereoDirectionFeatures& output,
                         std::string* error = nullptr) const;

    DirectionResult Estimate(uint64_t eventId,
                             SoundClass soundClass,
                             std::span<const float> interleaved,
                             const AudioProfile& profile,
                             const LocalizationTuning& tuning,
                             const DirectionCalibrationProfile* calibration = nullptr) const;

    DirectionResult EstimateFeatures(uint64_t eventId,
                                     SoundClass soundClass,
                                     const StereoDirectionFeatures& features,
                                     const AudioProfile& profile,
                                     const LocalizationTuning& tuning,
                                     const DirectionCalibrationProfile* calibration = nullptr) const;

private:
    Config m_config;

    static float FeatureDistance(const StereoDirectionFeatures& left,
                                 const StereoDirectionFeatures& right);
    static void AddCircularKernel(std::array<float, 24>& probabilities,
                                  float angleDegrees, float sigmaDegrees, float weight);
    static void Normalize(std::array<float, 24>& probabilities);
};

} // namespace EchoRadar
