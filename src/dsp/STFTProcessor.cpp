#include "STFTProcessor.h"
#include "WindowFunctions.h"
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {

STFTProcessor::STFTProcessor() : STFTProcessor(Config{}) {}

STFTProcessor::STFTProcessor(const Config& cfg) : m_cfg(cfg) {
    ValidateConfig();
    m_window = MakeHannWindow(m_cfg.fft_size);
    m_fft_input.resize(m_cfg.fft_size);
    m_fft_output.resize(GetBinCount());
    BuildFFTPlan();
}

STFTProcessor::~STFTProcessor() {
    DestroyFFTPlan();
}

void STFTProcessor::Reset() {
    m_left_samples.clear();
    m_right_samples.clear();
    m_next_window_start = 0;
    m_stream_start_sample = 0;
    m_next_frame_index = 0;
    m_ready_frames.clear();
}

void STFTProcessor::PushInterleaved(const float* samples, size_t frameCount) {
    if (samples == nullptr && frameCount > 0) {
        throw std::invalid_argument("PushInterleaved samples pointer is null");
    }

    const size_t previousSize = m_left_samples.size();
    m_left_samples.reserve(previousSize + frameCount);
    m_right_samples.reserve(previousSize + frameCount);

    for (size_t i = 0; i < frameCount; ++i) {
        m_left_samples.push_back(samples[i * 2]);
        m_right_samples.push_back(samples[i * 2 + 1]);
    }

    ProduceFrames();
    MaybeCompactBuffers();
}

size_t STFTProcessor::GetAvailableSTFTFrames() const {
    return m_ready_frames.size();
}

bool STFTProcessor::PopFrame(STFTFrame& outFrame) {
    if (m_ready_frames.empty()) {
        return false;
    }

    outFrame = std::move(m_ready_frames.front());
    m_ready_frames.pop_front();
    return true;
}

float STFTProcessor::BinToHz(uint32_t bin) const {
    return static_cast<float>(bin) * static_cast<float>(m_cfg.sample_rate) /
           static_cast<float>(m_cfg.fft_size);
}

void STFTProcessor::BuildFFTPlan() {
    m_fft_cfg = kiss_fftr_alloc(static_cast<int>(m_cfg.fft_size), 0, nullptr, nullptr);
    if (m_fft_cfg == nullptr) {
        throw std::runtime_error("Failed to allocate KissFFT real FFT plan");
    }
}

void STFTProcessor::DestroyFFTPlan() {
    if (m_fft_cfg != nullptr) {
        kiss_fftr_free(m_fft_cfg);
        m_fft_cfg = nullptr;
    }
}

void STFTProcessor::ValidateConfig() const {
    if (m_cfg.fft_size == 0) {
        throw std::invalid_argument("FFT size must be > 0");
    }
    if ((m_cfg.fft_size & (m_cfg.fft_size - 1)) != 0) {
        throw std::invalid_argument("FFT size must be a power of two");
    }
    if (m_cfg.hop_size == 0 || m_cfg.hop_size > m_cfg.fft_size) {
        throw std::invalid_argument("Hop size must be in range [1, fft_size]");
    }
    if (m_cfg.sample_rate == 0) {
        throw std::invalid_argument("Sample rate must be > 0");
    }
}

void STFTProcessor::ProduceFrames() {
    const size_t fftSize = m_cfg.fft_size;
    while (m_left_samples.size() >= (m_next_window_start + fftSize)) {
        STFTFrame frame;
        frame.frame_index = m_next_frame_index++;
        frame.start_sample = m_stream_start_sample + static_cast<uint64_t>(m_next_window_start);
        frame.fft_size = m_cfg.fft_size;
        frame.hop_size = m_cfg.hop_size;
        frame.sample_rate = m_cfg.sample_rate;

        ComputeChannelFrame(m_left_samples, m_next_window_start, frame.left);
        ComputeChannelFrame(m_right_samples, m_next_window_start, frame.right);

        m_ready_frames.push_back(std::move(frame));
        m_next_window_start += m_cfg.hop_size;
    }
}

void STFTProcessor::MaybeCompactBuffers() {
    constexpr size_t kCompactionThresholdFrames = 4096;
    if (m_next_window_start < kCompactionThresholdFrames) {
        return;
    }
    if (m_next_window_start == 0) {
        return;
    }

    m_left_samples.erase(m_left_samples.begin(), m_left_samples.begin() + m_next_window_start);
    m_right_samples.erase(m_right_samples.begin(), m_right_samples.begin() + m_next_window_start);
    m_stream_start_sample += static_cast<uint64_t>(m_next_window_start);
    m_next_window_start = 0;
}

void STFTProcessor::ComputeChannelFrame(const std::vector<float>& source,
                                        size_t start,
                                        STFTChannelFrame& out) {
    const uint32_t binCount = GetBinCount();
    out.spectrum.resize(binCount);
    out.magnitudes.resize(binCount);
    out.power.resize(binCount);

    for (uint32_t i = 0; i < m_cfg.fft_size; ++i) {
        m_fft_input[i] = source[start + i] * m_window[i];
    }

    kiss_fftr(m_fft_cfg,
              reinterpret_cast<const kiss_fft_scalar*>(m_fft_input.data()),
              m_fft_output.data());

    for (uint32_t bin = 0; bin < binCount; ++bin) {
        const float real = m_fft_output[bin].r;
        const float imag = m_fft_output[bin].i;
        const float mag = std::sqrt(real * real + imag * imag);

        out.spectrum[bin] = std::complex<float>(real, imag);
        out.magnitudes[bin] = mag;
        out.power[bin] = mag * mag;
    }
}

} // namespace EchoRadar
