#include "StereoDirectionEstimator.h"

#include <dsp/STFTProcessor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace EchoRadar {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr double kEpsilon = 1.0e-18;

float BandEdge(size_t index) {
    constexpr float low = 120.0f;
    constexpr float high = 20000.0f;
    const float position = static_cast<float>(index) /
        static_cast<float>(StereoDirectionFeatures::kBandCount);
    return low * std::pow(high / low, position);
}

size_t BandForFrequency(float frequency) {
    if (frequency < BandEdge(0) || frequency >= BandEdge(StereoDirectionFeatures::kBandCount)) {
        return StereoDirectionFeatures::kBandCount;
    }
    const float position = std::log(frequency / BandEdge(0)) /
        std::log(BandEdge(StereoDirectionFeatures::kBandCount) / BandEdge(0));
    return std::min(StereoDirectionFeatures::kBandCount - 1,
                    static_cast<size_t>(position * StereoDirectionFeatures::kBandCount));
}

float SafeDbRatio(double numerator, double denominator) {
    return static_cast<float>(10.0 * std::log10((numerator + 1.0e-12) /
                                                (denominator + 1.0e-12)));
}

struct EnvelopeAnalysis {
    std::vector<float> rms;
    std::vector<float> weights;
    size_t peakFrame{0};
    float peak{0.0f};
    float noise{0.0f};
    float peakToNoiseDb{0.0f};
    float activeFraction{0.0f};
};

EnvelopeAnalysis AnalyzeEnvelope(std::span<const float> interleaved,
                                 size_t smoothingFrames) {
    EnvelopeAnalysis result;
    const size_t frames = interleaved.size() / 2;
    if (frames == 0) return result;
    smoothingFrames = std::clamp<size_t>(smoothingFrames, 1, frames);
    result.rms.resize(frames);
    std::vector<double> prefix(frames + 1, 0.0);
    for (size_t frame = 0; frame < frames; ++frame) {
        const double left = interleaved[frame * 2];
        const double right = interleaved[frame * 2 + 1];
        prefix[frame + 1] = prefix[frame] + 0.5 * (left * left + right * right);
    }
    const size_t before = smoothingFrames / 2;
    const size_t after = smoothingFrames - before;
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t start = frame > before ? frame - before : 0;
        const size_t end = std::min(frames, frame + after);
        result.rms[frame] = static_cast<float>(
            std::sqrt(std::max(0.0, (prefix[end] - prefix[start]) /
                                      static_cast<double>(end - start))));
        if (result.rms[frame] > result.peak) {
            result.peak = result.rms[frame];
            result.peakFrame = frame;
        }
    }
    std::vector<float> ordered = result.rms;
    const size_t noiseIndex = std::min(ordered.size() - 1, ordered.size() / 5);
    std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(noiseIndex),
                     ordered.end());
    result.noise = ordered[noiseIndex];
    result.peakToNoiseDb = 20.0f * std::log10(
        (result.peak + 1.0e-9f) / (result.noise + 1.0e-9f));
    const float range = std::max(1.0e-9f, result.peak - result.noise);
    result.weights.resize(frames);
    size_t active = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const float normalized = std::clamp((result.rms[frame] - result.noise) / range,
                                            0.0f, 1.0f);
        result.weights[frame] = normalized * normalized;
        if (normalized >= 0.2f) ++active;
    }
    result.activeFraction = static_cast<float>(active) / static_cast<float>(frames);
    return result;
}

bool FiniteFeatures(const StereoDirectionFeatures& features) {
    const std::array<float, 10> scalars{
        features.broadbandIldDb, features.gccDelaySamples, features.gccPeak,
        features.gccSharpness, features.gccPeakToSidelobe, features.peakToNoiseDb,
        features.activeFrameFraction, features.stereoQuality, features.rms, 0.0f,
    };
    if (features.schemaVersion != StereoDirectionFeatures::kSchemaVersion ||
        !std::all_of(scalars.begin(), scalars.end(), [](float value) { return std::isfinite(value); })) {
        return false;
    }
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); });
    };
    return finite(features.bandIldDb) && finite(features.bandCoherence) &&
           finite(features.leftSpectralShape) && finite(features.rightSpectralShape);
}

std::vector<std::string> Split(const std::string& line, char separator) {
    std::vector<std::string> values;
    std::istringstream input(line);
    std::string value;
    while (std::getline(input, value, separator)) values.push_back(value);
    return values;
}

const char* ClassName(SoundClass soundClass) {
    return soundClass == SoundClass::Gunshot ? "gunshot" : "footstep";
}

SoundClass ParseClass(const std::string& text) {
    return text == "gunshot" ? SoundClass::Gunshot : SoundClass::Footstep;
}

void AddKernel(std::array<float, 24>& probabilities, float angleDegrees,
               float sigmaDegrees, float weight) {
    const float sigma = std::max(5.0f, sigmaDegrees);
    for (size_t index = 0; index < probabilities.size(); ++index) {
        const float distance = CircularDistanceDegrees(static_cast<float>(index) * 15.0f,
                                                       angleDegrees);
        probabilities[index] += weight * std::exp(-0.5f * distance * distance /
                                                   (sigma * sigma));
    }
}

void NormalizeProbabilities(std::array<float, 24>& probabilities) {
    const float sum = std::accumulate(probabilities.begin(), probabilities.end(), 0.0f);
    if (sum <= 1.0e-12f) {
        probabilities.fill(1.0f / static_cast<float>(probabilities.size()));
        return;
    }
    for (float& probability : probabilities) probability /= sum;
}

std::vector<float> DistanceVector(const StereoDirectionFeatures& features) {
    std::vector<float> values;
    values.reserve(2 + StereoDirectionFeatures::kBandCount * 4);
    values.push_back(features.broadbandIldDb);
    values.push_back(features.gccDelaySamples);
    values.insert(values.end(), features.bandIldDb.begin(), features.bandIldDb.end());
    values.insert(values.end(), features.bandCoherence.begin(), features.bandCoherence.end());
    for (float value : features.leftSpectralShape) values.push_back(value * 10.0f);
    for (float value : features.rightSpectralShape) values.push_back(value * 10.0f);
    return values;
}

float Median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    const float upper = values[middle];
    if (values.size() % 2 != 0) return upper;
    return 0.5f * (upper + *std::max_element(values.begin(),
                                             values.begin() + static_cast<std::ptrdiff_t>(middle)));
}

} // namespace

DirectionMapOutput DeterministicDirectionMapper::Map(
    SoundClass, const StereoDirectionFeatures& features, const AudioProfile& profile) const {
    DirectionMapOutput output;
    float ild = 0.0f;
    float ildWeight = 0.0f;
    for (size_t band = 3; band + 1 < StereoDirectionFeatures::kBandCount; ++band) {
        const float weight = 0.05f + features.bandCoherence[band] * features.bandCoherence[band];
        ild += weight * std::clamp(features.bandIldDb[band], -30.0f, 30.0f);
        ildWeight += weight;
    }
    if (ildWeight > 0.0f) ild /= ildWeight;
    ild = 0.35f * features.broadbandIldDb + 0.65f * ild;

    const float isolation = std::clamp(profile.leftRightIsolationPercent / 100.0f, 0.0f, 1.0f);
    const float levelEvidence = std::tanh(-ild / (9.0f * (1.0f + 0.65f * isolation)));
    const float timeEvidence = std::tanh(features.gccDelaySamples / 11.0f);
    float levelReliability = std::clamp(std::abs(ild) / 8.0f, 0.0f, 1.0f);
    float timeReliability = std::clamp(0.55f * features.gccSharpness +
                                           0.45f * features.gccPeakToSidelobe,
                                       0.0f, 1.0f);
    const bool conflicting = levelEvidence * timeEvidence < -0.04f;
    if (conflicting) {
        if (levelReliability >= timeReliability) timeReliability *= 0.2f;
        else levelReliability *= 0.2f;
    }
    const float side = std::clamp(
        (levelReliability * levelEvidence + timeReliability * timeEvidence) /
            std::max(0.001f, levelReliability + timeReliability),
        -1.0f, 1.0f);

    float mid = 0.0f;
    float high = 0.0f;
    float earShapeDifference = 0.0f;
    for (size_t band = 0; band < StereoDirectionFeatures::kBandCount; ++band) {
        const float averageShape = 0.5f * (features.leftSpectralShape[band] +
                                           features.rightSpectralShape[band]);
        if (band >= 6 && band < 17) mid += averageShape;
        if (band >= 17) high += averageShape;
        earShapeDifference += std::abs(features.leftSpectralShape[band] -
                                       features.rightSpectralShape[band]);
    }
    // This is intentionally evidence-driven rather than a fixed front prior.
    // Near zero, equal front/rear modes are retained and confidence falls.
    const float frontRearEvidence = std::tanh(2.5f * (mid - high));
    const float sideAngle = side * 90.0f;
    const float frontAngle = WrapDirectionDegrees(sideAngle);
    const float rearAngle = WrapDirectionDegrees(180.0f - sideAngle);
    const float frontWeight = 0.5f + 0.42f * frontRearEvidence;
    const float sigma = 16.0f + (1.0f - features.stereoQuality) * 42.0f;
    AddKernel(output.probabilities, frontAngle, sigma, frontWeight);
    AddKernel(output.probabilities, rearAngle, sigma, 1.0f - frontWeight);
    NormalizeProbabilities(output.probabilities);

    const float interauralEvidence = std::clamp(
        0.55f * std::abs(levelEvidence) + 0.45f * std::abs(timeEvidence), 0.0f, 1.0f);
    const float spectralReliability = std::clamp(earShapeDifference * 1.5f, 0.0f, 1.0f);
    const float frontRearReliability = std::abs(frontRearEvidence) *
        std::max(interauralEvidence, spectralReliability);
    output.directionalEvidence = std::clamp(
        (conflicting ? 0.55f : 1.0f) *
            std::max(interauralEvidence, 0.7f * frontRearReliability),
        0.0f, 1.0f);
    return output;
}

void DirectionCalibrationProfile::Clear() {
    m_audioProfileKey.clear();
    m_samples.clear();
}

void DirectionCalibrationProfile::SetAudioProfileKey(std::string key) {
    m_audioProfileKey = std::move(key);
}

bool DirectionCalibrationProfile::Matches(const AudioProfile& profile) const {
    return !m_samples.empty() && m_audioProfileKey == profile.StableKey();
}

void DirectionCalibrationProfile::AddSample(const DirectionCalibrationSample& sample) {
    if (!FiniteFeatures(sample.features) || !std::isfinite(sample.angleDegrees)) return;
    DirectionCalibrationSample safe = sample;
    safe.angleDegrees = WrapDirectionDegrees(safe.angleDegrees);
    m_samples.push_back(std::move(safe));
}

size_t DirectionCalibrationProfile::SampleCount(SoundClass soundClass) const {
    return static_cast<size_t>(std::count_if(
        m_samples.begin(), m_samples.end(),
        [soundClass](const auto& sample) { return sample.soundClass == soundClass; }));
}

bool DirectionCalibrationProfile::Save(const std::filesystem::path& path,
                                       std::string* error) const {
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError) {
            if (error) *error = "Could not create calibration directory";
            return false;
        }
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Could not open calibration file for writing";
        return false;
    }
    output << "echoradar-direction-calibration-v2\n";
    output << "profile\t" << std::quoted(m_audioProfileKey) << '\n';
    output << std::setprecision(9);
    for (const auto& sample : m_samples) {
        const auto& f = sample.features;
        output << "sample\t" << ClassName(sample.soundClass) << '\t' << sample.angleDegrees
               << '\t' << f.schemaVersion << '\t' << f.broadbandIldDb
               << '\t' << f.gccDelaySamples << '\t' << f.gccPeak
               << '\t' << f.gccSharpness << '\t' << f.gccPeakToSidelobe
               << '\t' << f.peakToNoiseDb << '\t' << f.activeFrameFraction
               << '\t' << f.stereoQuality << '\t' << f.rms;
        for (float value : f.bandIldDb) output << '\t' << value;
        for (float value : f.bandCoherence) output << '\t' << value;
        for (float value : f.leftSpectralShape) output << '\t' << value;
        for (float value : f.rightSpectralShape) output << '\t' << value;
        output << '\n';
    }
    output.flush();
    if (!output) {
        if (error) *error = "Could not finish writing calibration file";
        return false;
    }
    output.close();
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "Could not atomically publish calibration file";
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        if (error) *error = "Could not atomically publish calibration file";
        return false;
    }
#endif
    if (error) error->clear();
    return true;
}

bool DirectionCalibrationProfile::Load(const std::filesystem::path& path,
                                       std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "Calibration profile is unavailable";
        return false;
    }
    std::string line;
    if (!std::getline(input, line) || line != "echoradar-direction-calibration-v2") {
        if (error) *error = "Calibration profile feature schema is incompatible; recalibration is required";
        return false;
    }
    DirectionCalibrationProfile loaded;
    while (std::getline(input, line)) {
        if (line.rfind("profile\t", 0) == 0) {
            std::istringstream profileLine(line.substr(8));
            profileLine >> std::quoted(loaded.m_audioProfileKey);
            continue;
        }
        if (line.rfind("sample\t", 0) != 0) continue;
        const auto fields = Split(line, '\t');
        constexpr size_t expected = 13 + StereoDirectionFeatures::kBandCount * 4;
        if (fields.size() != expected) continue;
        try {
            DirectionCalibrationSample sample;
            sample.soundClass = ParseClass(fields[1]);
            sample.angleDegrees = std::stof(fields[2]);
            auto& f = sample.features;
            f.schemaVersion = static_cast<uint32_t>(std::stoul(fields[3]));
            f.broadbandIldDb = std::stof(fields[4]);
            f.gccDelaySamples = std::stof(fields[5]);
            f.gccPeak = std::stof(fields[6]);
            f.gccSharpness = std::stof(fields[7]);
            f.gccPeakToSidelobe = std::stof(fields[8]);
            f.peakToNoiseDb = std::stof(fields[9]);
            f.activeFrameFraction = std::stof(fields[10]);
            f.stereoQuality = std::stof(fields[11]);
            f.rms = std::stof(fields[12]);
            size_t cursor = 13;
            for (float& value : f.bandIldDb) value = std::stof(fields[cursor++]);
            for (float& value : f.bandCoherence) value = std::stof(fields[cursor++]);
            for (float& value : f.leftSpectralShape) value = std::stof(fields[cursor++]);
            for (float& value : f.rightSpectralShape) value = std::stof(fields[cursor++]);
            loaded.AddSample(sample);
        } catch (...) {
        }
    }
    if (loaded.m_audioProfileKey.empty()) {
        if (error) *error = "Calibration profile has no audio profile key";
        return false;
    }
    *this = std::move(loaded);
    if (error) error->clear();
    return true;
}

StereoDirectionEstimator::StereoDirectionEstimator()
    : StereoDirectionEstimator(Config{}) {}

StereoDirectionEstimator::StereoDirectionEstimator(Config config)
    : StereoDirectionEstimator(config, std::make_shared<DeterministicDirectionMapper>()) {}

StereoDirectionEstimator::StereoDirectionEstimator(
    Config config, std::shared_ptr<const DirectionMapper> mapper)
    : m_config(config), m_mapper(std::move(mapper)) {
    if (m_config.sampleRate == 0 || m_config.fftSize == 0 || m_config.hopSize == 0 ||
        m_config.maximumLagSamples == 0 || !m_mapper) {
        throw std::invalid_argument("Invalid stereo direction estimator configuration");
    }
}

bool StereoDirectionEstimator::SelectPeakWindow(
    std::span<const float> broadWindow,
    const LocalizationTuning::PeakWindowTuning& tuning,
    PeakWindowSelection& selection,
    std::vector<float>& selectedInterleaved,
    std::string* error) const {
    selection = {};
    selectedInterleaved.clear();
    if (broadWindow.size() % 2 != 0 || broadWindow.size() < 2) {
        if (error) *error = "Peak search input must be stereo";
        return false;
    }
    const size_t frames = broadWindow.size() / 2;
    const size_t smoothing = std::max<size_t>(1,
        static_cast<size_t>(tuning.envelopeSmoothingMs) * m_config.sampleRate / 1000u);
    const EnvelopeAnalysis envelope = AnalyzeEnvelope(broadWindow, smoothing);
    selection.peakFrame = envelope.peakFrame;
    selection.peakRms = envelope.peak;
    selection.noiseFloorRms = envelope.noise;
    selection.peakToNoiseDb = envelope.peakToNoiseDb;
    selection.activeFrameFraction = envelope.activeFraction;
    const size_t before = static_cast<size_t>(tuning.beforePeakMs) * m_config.sampleRate / 1000u;
    const size_t after = static_cast<size_t>(tuning.afterPeakMs) * m_config.sampleRate / 1000u;
    selection.startFrame = envelope.peakFrame > before ? envelope.peakFrame - before : 0;
    selection.endFrame = std::min(frames, envelope.peakFrame + after);
    selection.accepted = envelope.peakToNoiseDb >= tuning.minimumPeakToNoiseDb &&
        envelope.activeFraction >= tuning.minimumActiveFrameFraction &&
        selection.endFrame > selection.startFrame &&
        selection.endFrame - selection.startFrame >= m_config.fftSize;
    if (!selection.accepted) {
        if (error) *error = envelope.peakToNoiseDb < tuning.minimumPeakToNoiseDb
            ? "Direction peak is not sufficiently above the local noise floor"
            : "Direction peak has insufficient active-frame coverage";
        return false;
    }
    selectedInterleaved.assign(
        broadWindow.begin() + static_cast<std::ptrdiff_t>(selection.startFrame * 2),
        broadWindow.begin() + static_cast<std::ptrdiff_t>(selection.endFrame * 2));
    if (error) error->clear();
    return true;
}

bool StereoDirectionEstimator::ExtractFeatures(
    std::span<const float> interleaved,
    StereoDirectionFeatures& output,
    std::string* error) const {
    output = {};
    if (interleaved.size() < static_cast<size_t>(m_config.fftSize) * 2 ||
        interleaved.size() % 2 != 0) {
        if (error) *error = "Direction input must contain at least one stereo FFT window";
        return false;
    }
    const size_t frames = interleaved.size() / 2;
    const EnvelopeAnalysis envelope = AnalyzeEnvelope(
        interleaved, std::max<size_t>(1, m_config.sampleRate * 4u / 1000u));
    output.peakSample = envelope.peakFrame;
    output.clipEndSample = frames;
    output.peakToNoiseDb = envelope.peakToNoiseDb;
    output.activeFrameFraction = envelope.activeFraction;

    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    double weightSum = 0.0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const double weight = envelope.weights[frame];
        const double left = interleaved[frame * 2];
        const double right = interleaved[frame * 2 + 1];
        leftEnergy += weight * left * left;
        rightEnergy += weight * right * right;
        weightSum += weight;
    }
    output.rms = static_cast<float>(std::sqrt(
        (leftEnergy + rightEnergy) / std::max(1.0, 2.0 * weightSum)));
    if (output.rms < 1.0e-6f) {
        if (error) *error = "Direction input is silent or has no active peak";
        return false;
    }
    output.broadbandIldDb = SafeDbRatio(leftEnergy, rightEnergy);

    STFTProcessor processor({m_config.fftSize, m_config.hopSize, m_config.sampleRate});
    processor.PushInterleaved(interleaved.data(), frames);
    const size_t binCount = m_config.fftSize / 2 + 1;
    std::vector<std::complex<double>> cross(binCount);
    std::array<double, StereoDirectionFeatures::kBandCount> bandLeft{};
    std::array<double, StereoDirectionFeatures::kBandCount> bandRight{};
    std::array<std::complex<double>, StereoDirectionFeatures::kBandCount> bandCross{};
    double spectralWeight = 0.0;
    STFTFrame stft;
    while (processor.PopFrame(stft)) {
        const size_t center = std::min(frames - 1,
            static_cast<size_t>(stft.start_sample) + m_config.fftSize / 2);
        const double frameWeight = envelope.weights[center];
        if (frameWeight <= 1.0e-6) continue;
        spectralWeight += frameWeight;
        for (size_t bin = 1; bin < stft.left.spectrum.size(); ++bin) {
            const std::complex<double> value = static_cast<std::complex<double>>(
                stft.left.spectrum[bin] * std::conj(stft.right.spectrum[bin]));
            cross[bin] += frameWeight * value;
            const size_t band = BandForFrequency(processor.BinToHz(static_cast<uint32_t>(bin)));
            if (band >= StereoDirectionFeatures::kBandCount) continue;
            bandLeft[band] += frameWeight * stft.left.power[bin];
            bandRight[band] += frameWeight * stft.right.power[bin];
            bandCross[band] += frameWeight * value;
        }
    }
    if (spectralWeight <= 1.0e-9) {
        if (error) *error = "Direction input produced no active spectral frames";
        return false;
    }

    const int maximumLag = static_cast<int>(std::min<size_t>(m_config.maximumLagSamples,
                                                              frames / 4));
    std::vector<float> gcc(static_cast<size_t>(maximumLag * 2 + 1), 0.0f);
    size_t phatBins = 0;
    for (size_t bin = 1; bin + 1 < cross.size(); ++bin) {
        if (std::abs(cross[bin]) > 1.0e-12) ++phatBins;
    }
    for (int lag = -maximumLag; lag <= maximumLag; ++lag) {
        double score = 0.0;
        for (size_t bin = 1; bin + 1 < cross.size(); ++bin) {
            const double magnitude = std::abs(cross[bin]);
            if (magnitude <= 1.0e-12) continue;
            const double phase = 2.0 * kPi * static_cast<double>(bin * lag) /
                                 static_cast<double>(m_config.fftSize);
            score += std::real((cross[bin] / magnitude) *
                               std::complex<double>(std::cos(phase), std::sin(phase)));
        }
        gcc[static_cast<size_t>(lag + maximumLag)] = static_cast<float>(
            score / static_cast<double>(std::max<size_t>(1, phatBins)));
    }
    const auto bestIterator = std::max_element(gcc.begin(), gcc.end());
    const size_t bestIndex = static_cast<size_t>(std::distance(gcc.begin(), bestIterator));
    const int bestLag = static_cast<int>(bestIndex) - maximumLag;
    float refinedLag = static_cast<float>(bestLag);
    float neighborMean = *bestIterator;
    if (bestIndex > 0 && bestIndex + 1 < gcc.size()) {
        const float previous = gcc[bestIndex - 1];
        const float center = gcc[bestIndex];
        const float next = gcc[bestIndex + 1];
        neighborMean = 0.5f * (previous + next);
        const float curvature = previous - 2.0f * center + next;
        if (std::abs(curvature) > 1.0e-7f) {
            refinedLag += std::clamp(0.5f * (previous - next) / curvature, -0.5f, 0.5f);
        }
    }
    float sidelobe = -1.0f;
    for (size_t index = 0; index < gcc.size(); ++index) {
        if (std::abs(static_cast<int>(index) - static_cast<int>(bestIndex)) <= 2) continue;
        sidelobe = std::max(sidelobe, gcc[index]);
    }
    output.gccDelaySamples = refinedLag;
    output.gccPeak = std::clamp(*bestIterator, -1.0f, 1.0f);
    output.gccSharpness = std::clamp(
        (*bestIterator - neighborMean) / (std::abs(*bestIterator) + 1.0e-6f), 0.0f, 1.0f);
    output.gccPeakToSidelobe = std::clamp(
        (*bestIterator - sidelobe) / (std::abs(*bestIterator) + 1.0e-6f), 0.0f, 1.0f);

    const double totalLeft = std::accumulate(bandLeft.begin(), bandLeft.end(), 0.0);
    const double totalRight = std::accumulate(bandRight.begin(), bandRight.end(), 0.0);
    float coherenceMean = 0.0f;
    float coherenceWeight = 0.0f;
    for (size_t band = 0; band < StereoDirectionFeatures::kBandCount; ++band) {
        output.bandIldDb[band] = SafeDbRatio(bandLeft[band], bandRight[band]);
        output.bandCoherence[band] = static_cast<float>(
            std::abs(bandCross[band]) /
            std::sqrt(std::max(kEpsilon, bandLeft[band] * bandRight[band])));
        output.bandCoherence[band] = std::clamp(output.bandCoherence[band], 0.0f, 1.0f);
        output.leftSpectralShape[band] = static_cast<float>(
            bandLeft[band] / std::max(kEpsilon, totalLeft));
        output.rightSpectralShape[band] = static_cast<float>(
            bandRight[band] / std::max(kEpsilon, totalRight));
        const float energyWeight = static_cast<float>(
            (bandLeft[band] + bandRight[band]) /
            std::max(kEpsilon, totalLeft + totalRight));
        coherenceMean += energyWeight * output.bandCoherence[band];
        coherenceWeight += energyWeight;
    }
    coherenceMean /= std::max(1.0e-6f, coherenceWeight);
    const float peakClarity = std::clamp((output.peakToNoiseDb - 3.0f) / 18.0f, 0.0f, 1.0f);
    const float coverage = std::clamp(output.activeFrameFraction / 0.18f, 0.0f, 1.0f);
    const float gccQuality = std::sqrt(output.gccSharpness * output.gccPeakToSidelobe);
    output.stereoQuality = std::clamp(
        0.27f * peakClarity + 0.23f * coverage + 0.25f * coherenceMean +
            0.25f * gccQuality,
        0.0f, 1.0f);
    if (!FiniteFeatures(output)) {
        if (error) *error = "Direction feature extraction produced invalid values";
        return false;
    }
    if (error) error->clear();
    return true;
}

float StereoDirectionEstimator::RobustFeatureDistance(
    const StereoDirectionFeatures& query,
    const StereoDirectionFeatures& prototype,
    const std::vector<const DirectionCalibrationSample*>& classSamples) {
    const std::vector<float> queryValues = DistanceVector(query);
    const std::vector<float> prototypeValues = DistanceVector(prototype);
    double sum = 0.0;
    for (size_t dimension = 0; dimension < queryValues.size(); ++dimension) {
        std::vector<float> values;
        values.reserve(classSamples.size());
        for (const auto* sample : classSamples) values.push_back(DistanceVector(sample->features)[dimension]);
        const float median = Median(values);
        for (float& value : values) value = std::abs(value - median);
        float scale = 1.4826f * Median(std::move(values));
        if (dimension == 0) scale = std::max(scale, 1.5f);
        else if (dimension == 1) scale = std::max(scale, 0.75f);
        else if (dimension < 2 + StereoDirectionFeatures::kBandCount) scale = std::max(scale, 2.0f);
        else if (dimension < 2 + StereoDirectionFeatures::kBandCount * 2) scale = std::max(scale, 0.08f);
        else scale = std::max(scale, 0.05f);
        const float normalized = (queryValues[dimension] - prototypeValues[dimension]) / scale;
        sum += std::min(25.0f, normalized * normalized);
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(queryValues.size())));
}

void StereoDirectionEstimator::AddCircularKernel(std::array<float, 24>& probabilities,
                                                 float angleDegrees, float sigmaDegrees,
                                                 float weight) {
    AddKernel(probabilities, angleDegrees, sigmaDegrees, weight);
}

void StereoDirectionEstimator::Normalize(std::array<float, 24>& probabilities) {
    NormalizeProbabilities(probabilities);
}

DirectionResult StereoDirectionEstimator::Estimate(
    uint64_t eventId, SoundClass soundClass, std::span<const float> interleaved,
    const AudioProfile& profile, const LocalizationTuning& tuning,
    const DirectionCalibrationProfile* calibration) const {
    const auto start = std::chrono::steady_clock::now();
    StereoDirectionFeatures features;
    if (!ExtractFeatures(interleaved, features, nullptr)) {
        DirectionResult unavailable;
        unavailable.eventId = eventId;
        unavailable.status = DirectionStatus::AudioUnavailable;
        return unavailable;
    }
    DirectionResult result = EstimateFeatures(eventId, soundClass, features, profile,
                                              tuning, calibration);
    result.inferenceMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

DirectionResult StereoDirectionEstimator::EstimateFeatures(
    uint64_t eventId, SoundClass soundClass, const StereoDirectionFeatures& features,
    const AudioProfile& profile, const LocalizationTuning& tuning,
    const DirectionCalibrationProfile* calibration) const {
    DirectionResult result;
    result.eventId = eventId;
    result.featureSchemaVersion = features.schemaVersion;
    result.mapperVersion = m_mapper->Version();
    result.peakSample = features.peakSample;
    result.clipStartSample = features.clipStartSample;
    result.clipEndSample = features.clipEndSample;
    result.peakToNoiseDb = features.peakToNoiseDb;
    result.activeFrameFraction = features.activeFrameFraction;
    result.gccQuality = std::sqrt(features.gccSharpness * features.gccPeakToSidelobe);

    const DirectionMapOutput mapped = m_mapper->Map(soundClass, features, profile);
    result.probabilities = mapped.probabilities;
    float calibrationWeight = 0.0f;
    if (calibration && calibration->Matches(profile)) {
        std::vector<const DirectionCalibrationSample*> classSamples;
        for (const auto& sample : calibration->Samples()) {
            if (sample.soundClass == soundClass) classSamples.push_back(&sample);
        }
        std::vector<std::pair<float, const DirectionCalibrationSample*>> nearest;
        nearest.reserve(classSamples.size());
        for (const auto* sample : classSamples) {
            const float distance = RobustFeatureDistance(features, sample->features, classSamples);
            if (distance <= 4.0f) nearest.emplace_back(distance, sample);
        }
        const size_t keep = std::min<size_t>(8, nearest.size());
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(keep),
                          nearest.end(), [](const auto& left, const auto& right) {
                              return left.first < right.first;
                          });
        for (size_t index = 0; index < keep; ++index) {
            const auto& sample = nearest[index].second->features;
            const float sampleQuality = std::clamp(
                0.5f * sample.stereoQuality +
                    0.5f * std::sqrt(sample.gccSharpness * sample.gccPeakToSidelobe),
                0.0f, 1.0f);
            const float weight = sampleQuality * std::exp(
                -0.5f * nearest[index].first * nearest[index].first / (1.25f * 1.25f));
            AddCircularKernel(result.probabilities, nearest[index].second->angleDegrees,
                              10.0f + nearest[index].first * 7.0f, weight * 2.0f);
            calibrationWeight += weight;
        }
        if (calibrationWeight >= 0.25f) result.profileSource = DirectionProfileSource::Calibrated;
    }
    Normalize(result.probabilities);

    const auto primaryIterator = std::max_element(result.probabilities.begin(),
                                                   result.probabilities.end());
    const size_t primaryIndex = static_cast<size_t>(
        std::distance(result.probabilities.begin(), primaryIterator));
    const size_t previousIndex = (primaryIndex + 23) % 24;
    const size_t nextIndex = (primaryIndex + 1) % 24;
    const float curvature = result.probabilities[previousIndex] -
        2.0f * result.probabilities[primaryIndex] + result.probabilities[nextIndex];
    float offset = 0.0f;
    if (std::abs(curvature) > 1.0e-7f) {
        offset = std::clamp(0.5f * (result.probabilities[previousIndex] -
                                    result.probabilities[nextIndex]) / curvature,
                            -0.5f, 0.5f);
    }
    result.primaryAngleDegrees = WrapDirectionDegrees(
        (static_cast<float>(primaryIndex) + offset) * 15.0f);

    float secondaryProbability = 0.0f;
    size_t secondaryIndex = primaryIndex;
    for (size_t index = 0; index < 24; ++index) {
        if (CircularDistanceDegrees(static_cast<float>(index) * 15.0f,
                                    result.primaryAngleDegrees) <
            tuning.secondaryMinimumSeparationDegrees) continue;
        if (result.probabilities[index] > secondaryProbability) {
            secondaryProbability = result.probabilities[index];
            secondaryIndex = index;
        }
    }
    if (tuning.showSecondaryDirection && *primaryIterator > 0.0f &&
        secondaryProbability / *primaryIterator >= tuning.secondaryRatio) {
        result.secondaryAngleDegrees = static_cast<float>(secondaryIndex) * 15.0f;
        result.secondaryConfidence = std::clamp(
            secondaryProbability / *primaryIterator * features.stereoQuality, 0.0f, 1.0f);
    }

    double x = 0.0;
    double y = 0.0;
    double entropy = 0.0;
    for (size_t index = 0; index < 24; ++index) {
        const double probability = result.probabilities[index];
        const double radians = static_cast<double>(index) * 15.0 * kPi / 180.0;
        x += probability * std::cos(radians);
        y += probability * std::sin(radians);
        if (probability > 1.0e-12) entropy -= probability * std::log(probability);
    }
    entropy /= std::log(24.0);
    const float resultant = static_cast<float>(std::clamp(std::sqrt(x * x + y * y), 0.0, 1.0));
    result.uncertaintyDegrees = std::clamp(10.0f + (1.0f - resultant) * 100.0f,
                                           10.0f, 110.0f);
    const float distributionCertainty = std::clamp(1.0f - static_cast<float>(entropy),
                                                    0.0f, 1.0f);
    const float calibrationBoost = std::clamp(calibrationWeight / 2.0f, 0.0f, 1.0f);
    result.confidence = std::clamp(
        features.stereoQuality *
            std::max(mapped.directionalEvidence * (0.35f + 0.65f * distributionCertainty),
                     calibrationBoost),
        0.0f, 1.0f);
    result.status = result.confidence >= tuning.minimumConfidence
        ? DirectionStatus::Estimated : DirectionStatus::LowConfidence;
    return result;
}

} // namespace EchoRadar
