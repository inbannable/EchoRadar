#include "STFTProcessor.h"
#include "WindowFunctions.h"
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {

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

Spectrogram STFTProcessor::Process(const std::vector<float>& samples) {
    std::vector<float> interleaved(samples.size() * 2, 0.0f);
    for (size_t i = 0; i < samples.size(); ++i) {
        interleaved[i * 2] = samples[i];
        interleaved[i * 2 + 1] = samples[i];
    }

    PushInterleaved(interleaved.data(), samples.size());

    Spectrogram monoSpec;
    monoSpec.fft_size = m_cfg.fft_size;
    monoSpec.hop_size = m_cfg.hop_size;
    monoSpec.sample_rate = m_cfg.sample_rate;

    STFTFrame frame;
    while (PopFrame(frame)) {
        monoSpec.magnitude.push_back(frame.left.magnitudes);
    }

    return monoSpec;
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

std::pair<Spectrogram, Spectrogram>
STFTProcessor::ProcessStereo(const AudioFrame& frame) {
    if (frame.left.size() != frame.right.size()) {
        throw std::invalid_argument("AudioFrame left/right channels must have equal length");
    }

    const size_t frameCount = frame.left.size();
    std::vector<float> interleaved(frameCount * 2);
    for (size_t i = 0; i < frameCount; ++i) {
        interleaved[i * 2] = frame.left[i];
        interleaved[i * 2 + 1] = frame.right[i];
    }

    PushInterleaved(interleaved.data(), frameCount);

    Spectrogram leftSpec;
    leftSpec.fft_size = m_cfg.fft_size;
    leftSpec.hop_size = m_cfg.hop_size;
    leftSpec.sample_rate = m_cfg.sample_rate;

    Spectrogram rightSpec = leftSpec;

    STFTFrame stftFrame;
    while (PopFrame(stftFrame)) {
        leftSpec.magnitude.push_back(stftFrame.left.magnitudes);
        rightSpec.magnitude.push_back(stftFrame.right.magnitudes);
    }

    return {std::move(leftSpec), std::move(rightSpec)};
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
