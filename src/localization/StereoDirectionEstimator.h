#pragma once

#include "LocalizationTypes.h"

#include <recognition/SoundRecognitionTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace EchoRadar {

struct StereoDirectionFeatures {
    static constexpr uint32_t kSchemaVersion = 2;
    static constexpr size_t kBandCount = 24;

    uint32_t schemaVersion{kSchemaVersion};
    float broadbandIldDb{0.0f};
    float gccDelaySamples{0.0f};
    float gccPeak{0.0f};
    float gccSharpness{0.0f};
    float gccPeakToSidelobe{0.0f};
    float peakToNoiseDb{0.0f};
    float activeFrameFraction{0.0f};
    float stereoQuality{0.0f};
    float rms{0.0f};
    uint64_t peakSample{0};
    uint64_t clipStartSample{0};
    uint64_t clipEndSample{0};
    std::array<float, kBandCount> bandIldDb{};
    std::array<float, kBandCount> bandCoherence{};
    std::array<float, kBandCount> leftSpectralShape{};
    std::array<float, kBandCount> rightSpectralShape{};
};

struct PeakWindowSelection {
    size_t peakFrame{0};
    size_t startFrame{0};
    size_t endFrame{0};
    float peakRms{0.0f};
    float noiseFloorRms{0.0f};
    float peakToNoiseDb{0.0f};
    float activeFrameFraction{0.0f};
    bool accepted{false};
};

struct DirectionCalibrationSample {
    SoundClass soundClass{SoundClass::Footstep};
    float angleDegrees{0.0f};
    StereoDirectionFeatures features;
};

class DirectionCalibrationProfile {
public:
    static constexpr uint32_t kSchemaVersion = 2;
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

struct DirectionMapOutput {
    std::array<float, 24> probabilities{};
    float directionalEvidence{0.0f};
};

class DirectionMapper {
public:
    virtual ~DirectionMapper() = default;
    virtual uint32_t Version() const = 0;
    virtual DirectionMapOutput Map(SoundClass soundClass,
                                   const StereoDirectionFeatures& features,
                                   const AudioProfile& profile) const = 0;
};

class DeterministicDirectionMapper final : public DirectionMapper {
public:
    static constexpr uint32_t kVersion = 2;
    uint32_t Version() const override { return kVersion; }
    DirectionMapOutput Map(SoundClass soundClass,
                           const StereoDirectionFeatures& features,
                           const AudioProfile& profile) const override;
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
    StereoDirectionEstimator(Config config, std::shared_ptr<const DirectionMapper> mapper);

    bool SelectPeakWindow(std::span<const float> broadWindow,
                          const LocalizationTuning::PeakWindowTuning& tuning,
                          PeakWindowSelection& selection,
                          std::vector<float>& selectedInterleaved,
                          std::string* error = nullptr) const;

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
    std::shared_ptr<const DirectionMapper> m_mapper;

    static float RobustFeatureDistance(
        const StereoDirectionFeatures& query,
        const StereoDirectionFeatures& prototype,
        const std::vector<const DirectionCalibrationSample*>& classSamples);
    static void AddCircularKernel(std::array<float, 24>& probabilities,
                                  float angleDegrees, float sigmaDegrees, float weight);
    static void Normalize(std::array<float, 24>& probabilities);
};

} // namespace EchoRadar
