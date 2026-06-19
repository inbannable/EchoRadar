#include "STFTProcessor.h"
#include <cmath>
#include <stdexcept>

// KissFFT is pulled in via FetchContent; include here once available.
// #include <kiss_fft.h>

namespace EchoRadar {

namespace {
    constexpr float kPi = 3.14159265358979323846f;
}

STFTProcessor::STFTProcessor(Config cfg) : m_cfg(cfg) {
    if (cfg.fft_size == 0 || (cfg.fft_size & (cfg.fft_size - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two");
    BuildWindow();
    BuildFFTPlan();
}

STFTProcessor::~STFTProcessor() {
    DestroyFFTPlan();
}

void STFTProcessor::BuildWindow() {
    m_window.resize(m_cfg.fft_size);
    for (uint32_t i = 0; i < m_cfg.fft_size; ++i) {
        m_window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (m_cfg.fft_size - 1)));
    }
}

void STFTProcessor::BuildFFTPlan() {
    // TODO (Milestone 3): kiss_fft_alloc(m_cfg.fft_size, 0, nullptr, nullptr)
    m_fft_cfg = nullptr;
}

void STFTProcessor::DestroyFFTPlan() {
    // TODO (Milestone 3): kiss_fft_free(m_fft_cfg)
    m_fft_cfg = nullptr;
}

Spectrogram STFTProcessor::Process(const std::vector<float>& samples) {
    Spectrogram spec;
    spec.fft_size   = m_cfg.fft_size;
    spec.hop_size   = m_cfg.hop_size;
    spec.sample_rate = m_cfg.sample_rate;

    // TODO (Milestone 3): sliding window STFT using KissFFT.
    // Stub: return one empty frame so downstream code doesn't crash.
    uint32_t n_bins = m_cfg.fft_size / 2 + 1;
    spec.magnitude.push_back(std::vector<float>(n_bins, 0.0f));
    return spec;
}

std::pair<Spectrogram, Spectrogram>
STFTProcessor::ProcessStereo(const AudioFrame& frame) {
    return { Process(frame.left), Process(frame.right) };
}

float STFTProcessor::BinToHz(uint32_t bin) const {
    return static_cast<float>(bin) * m_cfg.sample_rate / m_cfg.fft_size;
}

} // namespace EchoRadar
