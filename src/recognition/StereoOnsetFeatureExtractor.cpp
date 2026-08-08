#include "StereoOnsetFeatureExtractor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace EchoRadar {
namespace {

double HzToMel(double hz) {
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

double MelToHz(double mel) {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

} // namespace

StereoOnsetFeatureExtractor::StereoOnsetFeatureExtractor()
    : StereoOnsetFeatureExtractor(Config{}) {}

StereoOnsetFeatureExtractor::StereoOnsetFeatureExtractor(const Config& config)
    : m_config(config),
      m_stft(STFTProcessor::Config{config.fftSize, config.hopSize, config.sampleRate}) {
    if (config.sampleRate != 48000 || config.fftSize != 1024 || config.hopSize != 240 ||
        config.melBins != StereoOnsetFeatureFrame::kMelBins || config.minHz < 0.0f ||
        config.maxHz <= config.minHz || config.maxHz > config.sampleRate * 0.5f ||
        config.pcenSmoothing <= 0.0f || config.pcenSmoothing > 1.0f ||
        config.pcenAlpha < 0.0f || config.pcenAlpha > 1.0f || config.pcenDelta <= 0.0f ||
        config.pcenRoot <= 0.0f || config.pcenRoot > 1.0f || config.pcenEpsilon <= 0.0f ||
        config.activitySmoothingFrames == 0) {
        throw std::invalid_argument("Invalid stereo-onset-v4 feature configuration");
    }
    BuildFilters();
}

void StereoOnsetFeatureExtractor::Reset() {
    m_stft.Reset();
    m_pcenSmooth.fill(0.0f);
    m_pcenInitialized = false;
    m_activityInitialized = false;
    m_activityState = 0.0f;
    m_ready.clear();
}

void StereoOnsetFeatureExtractor::BuildFilters() {
    const uint32_t binCount = m_stft.GetBinCount();
    m_filters.assign(static_cast<size_t>(m_config.melBins) * binCount, 0.0f);
    const double minMel = HzToMel(m_config.minHz);
    const double maxMel = HzToMel(m_config.maxHz);
    std::vector<double> points(m_config.melBins + 2);
    for (size_t index = 0; index < points.size(); ++index) {
        const double mel = minMel + (maxMel - minMel) * static_cast<double>(index) /
            static_cast<double>(points.size() - 1);
        points[index] = MelToHz(mel);
    }
    for (uint32_t mel = 0; mel < m_config.melBins; ++mel) {
        const double left = points[mel];
        const double center = points[mel + 1];
        const double right = points[mel + 2];
        for (uint32_t bin = 0; bin < binCount; ++bin) {
            const double frequency = static_cast<double>(bin) * m_config.sampleRate / m_config.fftSize;
            const double rising = (frequency - left) / (center - left);
            const double falling = (right - frequency) / (right - center);
            m_filters[static_cast<size_t>(mel) * binCount + bin] =
                static_cast<float>(std::max(0.0, std::min(rising, falling)));
        }
    }
}

void StereoOnsetFeatureExtractor::PushInterleaved(const float* stereoSamples, size_t frameCount) {
    if (stereoSamples == nullptr && frameCount != 0) {
        throw std::invalid_argument("StereoOnsetFeatureExtractor received null PCM");
    }
    m_stft.PushInterleaved(stereoSamples, frameCount);
    const uint32_t binCount = m_stft.GetBinCount();
    const float normalization = 1.0f /
        static_cast<float>(static_cast<uint64_t>(m_config.fftSize) * m_config.fftSize);
    const float activityAlpha = 2.0f /
        static_cast<float>(m_config.activitySmoothingFrames + 1u);

    STFTFrame spectrum;
    while (m_stft.PopFrame(spectrum)) {
        StereoOnsetFeatureFrame frame;
        frame.startSample = spectrum.start_sample;
        frame.endSample = spectrum.start_sample + m_config.fftSize;
        float absoluteMean = 0.0f;
        for (uint32_t mel = 0; mel < m_config.melBins; ++mel) {
            float leftMel = 0.0f;
            float rightMel = 0.0f;
            std::complex<float> crossMel{};
            const float* filter = m_filters.data() + static_cast<size_t>(mel) * binCount;
            for (uint32_t bin = 0; bin < binCount; ++bin) {
                const float weight = filter[bin];
                leftMel += weight * spectrum.left.power[bin] * normalization;
                rightMel += weight * spectrum.right.power[bin] * normalization;
                crossMel += weight * spectrum.left.spectrum[bin] *
                    std::conj(spectrum.right.spectrum[bin]) * normalization;
            }
            leftMel = std::max(0.0f, leftMel);
            rightMel = std::max(0.0f, rightMel);
            const float meanEnergy = 0.5f * (leftMel + rightMel);
            if (!m_pcenInitialized) m_pcenSmooth[mel] = meanEnergy;
            else {
                m_pcenSmooth[mel] = (1.0f - m_config.pcenSmoothing) * m_pcenSmooth[mel] +
                    m_config.pcenSmoothing * meanEnergy;
            }
            const float pcen = std::pow(
                meanEnergy / std::pow(m_config.pcenEpsilon + m_pcenSmooth[mel],
                                      m_config.pcenAlpha) + m_config.pcenDelta,
                m_config.pcenRoot) - std::pow(m_config.pcenDelta, m_config.pcenRoot);
            const float absolute = std::clamp(
                10.0f * std::log10(std::max(meanEnergy, 1e-10f)), -100.0f, 0.0f);
            const float ild = std::clamp(
                10.0f * std::log10((leftMel + 1e-10f) / (rightMel + 1e-10f)) / 30.0f,
                -1.0f, 1.0f);
            const float coherenceDenominator = std::sqrt(leftMel * rightMel) + 1e-10f;
            const std::complex<float> coherence = crossMel / coherenceDenominator;
            frame.values[mel] = pcen;
            frame.values[m_config.melBins + mel] = absolute;
            frame.values[2 * m_config.melBins + mel] = ild;
            frame.values[3 * m_config.melBins + mel] =
                std::clamp(coherence.real(), -1.0f, 1.0f);
            frame.values[4 * m_config.melBins + mel] =
                std::clamp(coherence.imag(), -1.0f, 1.0f);
            absoluteMean += absolute;
        }
        m_pcenInitialized = true;
        const float instantaneous = std::clamp(
            (absoluteMean / static_cast<float>(m_config.melBins) + 80.0f) / 60.0f,
            0.0f, 1.0f);
        if (!m_activityInitialized) m_activityState = instantaneous;
        else m_activityState = activityAlpha * instantaneous +
            (1.0f - activityAlpha) * m_activityState;
        m_activityInitialized = true;
        frame.sceneActivity = m_activityState;
        m_ready.push_back(std::move(frame));
    }
}

bool StereoOnsetFeatureExtractor::PopFrame(StereoOnsetFeatureFrame& frame) {
    if (m_ready.empty()) return false;
    frame = std::move(m_ready.front());
    m_ready.pop_front();
    return true;
}

} // namespace EchoRadar
