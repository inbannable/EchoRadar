#include "LogMelExtractor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {
namespace {

float HzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

float MelToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

} // namespace

LogMelExtractor::LogMelExtractor(const Config& config)
    : m_config(config),
      m_stft(STFTProcessor::Config{config.fftSize, config.hopSize, config.sampleRate}) {
    if (config.melBins == 0 || config.minHz < 0.0f || config.maxHz <= config.minHz ||
        config.maxHz > static_cast<float>(config.sampleRate) * 0.5f || config.logScale <= 0.0f ||
        config.pcenSmoothing <= 0.0f || config.pcenSmoothing > 1.0f ||
        config.pcenAlpha < 0.0f || config.pcenAlpha > 1.0f || config.pcenDelta <= 0.0f ||
        config.pcenRoot <= 0.0f || config.pcenRoot > 1.0f || config.pcenEpsilon <= 0.0f) {
        throw std::invalid_argument("Invalid log-mel configuration");
    }
    BuildFilters();
    m_pcenSmooth.assign(config.melBins, 0.0f);
}

void LogMelExtractor::Reset() {
    m_stft.Reset();
    m_ready.clear();
    std::fill(m_pcenSmooth.begin(), m_pcenSmooth.end(), 0.0f);
    m_pcenInitialized = false;
}

void LogMelExtractor::BuildFilters() {
    const uint32_t binCount = m_stft.GetBinCount();
    m_filters.assign(static_cast<size_t>(m_config.melBins) * binCount, 0.0f);
    const float minMel = HzToMel(m_config.minHz);
    const float maxMel = HzToMel(m_config.maxHz);
    std::vector<float> points(m_config.melBins + 2);
    for (uint32_t i = 0; i < points.size(); ++i) {
        const float mel = minMel + (maxMel - minMel) * static_cast<float>(i) /
            static_cast<float>(points.size() - 1);
        points[i] = MelToHz(mel);
    }

    for (uint32_t mel = 0; mel < m_config.melBins; ++mel) {
        const float left = points[mel];
        const float center = points[mel + 1];
        const float right = points[mel + 2];
        for (uint32_t bin = 0; bin < binCount; ++bin) {
            const float hz = static_cast<float>(bin) * m_config.sampleRate / m_config.fftSize;
            float weight = 0.0f;
            if (hz >= left && hz <= center && center > left) weight = (hz - left) / (center - left);
            else if (hz > center && hz <= right && right > center) weight = (right - hz) / (right - center);
            m_filters[static_cast<size_t>(mel) * binCount + bin] = std::max(0.0f, weight);
        }
    }
}

void LogMelExtractor::PushInterleaved(const float* stereoSamples, size_t frameCount) {
    if (stereoSamples == nullptr && frameCount != 0) {
        throw std::invalid_argument("LogMelExtractor received null PCM");
    }
    if (m_config.mode == Mode::LogMelV1) {
        m_monoStereoScratch.resize(frameCount * 2);
        for (size_t i = 0; i < frameCount; ++i) {
            const float mono = 0.5f * (stereoSamples[i * 2] + stereoSamples[i * 2 + 1]);
            m_monoStereoScratch[i * 2] = mono;
            m_monoStereoScratch[i * 2 + 1] = mono;
        }
        m_stft.PushInterleaved(m_monoStereoScratch.data(), frameCount);
    } else {
        m_stft.PushInterleaved(stereoSamples, frameCount);
    }

    const uint32_t binCount = m_stft.GetBinCount();
    const float powerNormalization = 1.0f /
        static_cast<float>(static_cast<uint64_t>(m_config.fftSize) * m_config.fftSize);
    STFTFrame stftFrame;
    while (m_stft.PopFrame(stftFrame)) {
        LogMelFrame frame;
        frame.startSample = stftFrame.start_sample;
        frame.values.resize(m_config.melBins * GetOutputChannels());
        for (uint32_t mel = 0; mel < m_config.melBins; ++mel) {
            float energy = 0.0f;
            const float* filter = m_filters.data() + static_cast<size_t>(mel) * binCount;
            for (uint32_t bin = 0; bin < binCount; ++bin) {
                const float power = m_config.mode == Mode::LogMelV1
                    ? stftFrame.left.power[bin]
                    : 0.5f * (stftFrame.left.power[bin] + stftFrame.right.power[bin]);
                energy += filter[bin] * power * powerNormalization;
            }
            energy = std::max(0.0f, energy);
            if (m_config.mode == Mode::LogMelV1) {
                frame.values[mel] = std::log1p(m_config.logScale * energy);
                continue;
            }
            if (!m_pcenInitialized) m_pcenSmooth[mel] = energy;
            else m_pcenSmooth[mel] = (1.0f - m_config.pcenSmoothing) * m_pcenSmooth[mel] +
                                     m_config.pcenSmoothing * energy;
            const float normalized = energy /
                std::pow(m_config.pcenEpsilon + m_pcenSmooth[mel], m_config.pcenAlpha);
            frame.values[mel] = std::pow(normalized + m_config.pcenDelta, m_config.pcenRoot) -
                                std::pow(m_config.pcenDelta, m_config.pcenRoot);
            const float db = 10.0f * std::log10(std::max(energy, 1e-10f));
            frame.values[m_config.melBins + mel] = std::clamp(db, -100.0f, 0.0f);
        }
        m_pcenInitialized = true;
        m_ready.push_back(std::move(frame));
    }
}

bool LogMelExtractor::PopFrame(LogMelFrame& frame) {
    if (m_ready.empty()) return false;
    frame = std::move(m_ready.front());
    m_ready.pop_front();
    return true;
}

} // namespace EchoRadar
