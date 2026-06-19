#pragma once
#include <cstdint>
#include <vector>

namespace EchoRadar {

// ── Audio ──────────────────────────────────────────────────────────────────────

struct AudioFrame {
    std::vector<float> left;
    std::vector<float> right;
    uint64_t           timestamp_ms{0};
    uint32_t           sample_rate{48000};
};

// ── DSP ───────────────────────────────────────────────────────────────────────

struct Spectrogram {
    // [frame_index][bin_index]  magnitude spectrum
    std::vector<std::vector<float>> magnitude;
    uint32_t fft_size{1024};
    uint32_t hop_size{512};
    uint32_t sample_rate{48000};
};

// ── Events ────────────────────────────────────────────────────────────────────

struct GunshotEvent {
    float    confidence{0.0f};  // 0..1
    uint64_t timestamp{0};      // ms since epoch
};

struct FootstepEvent {
    float    confidence{0.0f};
    uint64_t timestamp{0};
};

// ── Features ─────────────────────────────────────────────────────────────────

struct FeatureVector {
    float ild{0.0f};              // Interaural Level Difference  (dB)
    float itd{0.0f};              // Interaural Time Difference   (samples)
    float spectral_centroid{0.0f};
    float spectral_rolloff{0.0f};
    std::vector<float> energy_bands; // sub-band energy distribution
    uint64_t timestamp{0};
};

// ── Localization ──────────────────────────────────────────────────────────────

struct DirectionEstimate {
    float    angle{0.0f};       // 0..360 degrees  (0 = forward/north)
    float    confidence{0.0f};  // 0..1
    uint64_t timestamp{0};
};

} // namespace EchoRadar
