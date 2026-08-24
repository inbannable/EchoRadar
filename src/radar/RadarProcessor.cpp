#include "RadarProcessor.h"

#include <kiss_fftr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace EchoRadar {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegreesToRadians = kPi / 180.0f;
constexpr float kRadiansToDegrees = 180.0f / kPi;
constexpr float kMinimumDirectionalRatio = 1.0e-4f;
constexpr float kMinimumPower = 1.0e-12f;
constexpr uint32_t kMaximumChannels = 8;
constexpr size_t kFrameQueueCapacity = 64;

constexpr std::array<float, kRadarCustomCurvePointCount> kFootstepCurveDb{{
    -18.0f, -12.0f, -4.0f, 2.0f,
      6.0f,   6.0f,  5.0f, 4.0f,
      2.0f,   0.0f, -3.0f, -6.0f,
    -10.0f, -14.0f, -18.0f, -24.0f,
}};

constexpr std::array<float, kRadarCustomCurvePointCount> kGunshotCurveDb{{
    -24.0f, -20.0f, -16.0f, -12.0f,
     -8.0f,  -4.0f,  -1.0f,   2.0f,
      4.0f,   6.0f,   6.0f,   5.0f,
      3.0f,   0.0f,  -4.0f, -10.0f,
}};

float ClampFinite(float value, float minimum, float maximum, float fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

float NormalizeDegrees(float degrees) {
    if (!std::isfinite(degrees)) {
        return 0.0f;
    }
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

float CircularDistanceDegrees(float left, float right) {
    const float difference = std::fabs(NormalizeDegrees(left) - NormalizeDegrees(right));
    return std::min(difference, 360.0f - difference);
}

float PowerToDbfs(float power) {
    if (!std::isfinite(power) || power <= std::pow(10.0f, kRadarSilenceDbfs / 10.0f)) {
        return kRadarSilenceDbfs;
    }
    return std::clamp(10.0f * std::log10(power), kRadarSilenceDbfs, 0.0f);
}

float DbfsToPower(float dbfs) {
    if (!std::isfinite(dbfs) || dbfs <= kRadarSilenceDbfs) {
        return 0.0f;
    }
    return std::pow(10.0f, std::min(dbfs, 0.0f) / 10.0f);
}

bool LayoutsEqual(const AudioChannelLayout& left, const AudioChannelLayout& right) {
    if (left.channelCount != right.channelCount ||
        left.channelMask != right.channelMask ||
        left.kind != right.kind) {
        return false;
    }
    for (size_t index = 0; index < left.roles.size(); ++index) {
        if (left.roles[index] != right.roles[index]) {
            return false;
        }
    }
    return true;
}

struct ValidatedLayout {
    std::array<bool, kMaximumChannels> directional{};
    std::array<float, kMaximumChannels> directionX{};
    std::array<float, kMaximumChannels> directionY{};
    uint32_t directionalChannelCount{0};
};

uint32_t WindowsSpeakerBitForRole(AudioChannelRole role) {
    switch (role) {
    case AudioChannelRole::FrontLeft: return WindowsSpeaker::FrontLeft;
    case AudioChannelRole::FrontRight: return WindowsSpeaker::FrontRight;
    case AudioChannelRole::FrontCenter: return WindowsSpeaker::FrontCenter;
    case AudioChannelRole::Lfe: return WindowsSpeaker::Lfe;
    case AudioChannelRole::BackLeft: return WindowsSpeaker::BackLeft;
    case AudioChannelRole::BackRight: return WindowsSpeaker::BackRight;
    case AudioChannelRole::SideLeft: return WindowsSpeaker::SideLeft;
    case AudioChannelRole::SideRight: return WindowsSpeaker::SideRight;
    case AudioChannelRole::Unknown: return 0u;
    }
    return 0u;
}

std::optional<float> RadarRoleAzimuthDegrees(AudioChannelLayoutKind kind,
                                             AudioChannelRole role) {
    const bool is51 = kind == AudioChannelLayoutKind::Surround51Back ||
                      kind == AudioChannelLayoutKind::Surround51Side;
    switch (role) {
    case AudioChannelRole::FrontLeft: return 330.0f;
    case AudioChannelRole::FrontRight: return 30.0f;
    case AudioChannelRole::FrontCenter: return 0.0f;
    case AudioChannelRole::SideLeft: return is51 ? 250.0f : 270.0f;
    case AudioChannelRole::SideRight: return is51 ? 110.0f : 90.0f;
    case AudioChannelRole::BackLeft: return is51 ? 250.0f : 210.0f;
    case AudioChannelRole::BackRight: return is51 ? 110.0f : 150.0f;
    case AudioChannelRole::Lfe:
    case AudioChannelRole::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

RadarRuntimeStatus ValidateLayout(const AudioChannelLayout& layout,
                                  ValidatedLayout* output) {
    const bool is51 = layout.kind == AudioChannelLayoutKind::Surround51Back ||
                      layout.kind == AudioChannelLayoutKind::Surround51Side;
    const bool is71 = layout.kind == AudioChannelLayoutKind::Surround71;
    if (!is51 && !is71) {
        return RadarRuntimeStatus::UnsupportedLayout;
    }
    if ((is51 && layout.channelCount != 6u) ||
        (is71 && layout.channelCount != 8u) ||
        layout.channelCount > layout.roles.size()) {
        return RadarRuntimeStatus::MalformedInput;
    }
    const uint32_t expectedMask =
        layout.kind == AudioChannelLayoutKind::Surround51Back
            ? WindowsSpeaker::Surround51Back
        : layout.kind == AudioChannelLayoutKind::Surround51Side
            ? WindowsSpeaker::Surround51Side
            : WindowsSpeaker::Surround71;
    if (layout.channelMask != expectedMask ||
        !IsValidWindowsSpeakerMask(layout.channelCount, layout.channelMask)) {
        return RadarRuntimeStatus::MalformedInput;
    }

    ValidatedLayout validated;
    uint32_t lfeCount = 0;
    uint32_t rolesMask = 0;
    for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
        const AudioChannelRole role = layout.roles[channel];
        const uint32_t speakerBit = WindowsSpeakerBitForRole(role);
        if (speakerBit == 0u || (rolesMask & speakerBit) != 0u ||
            (layout.channelMask & speakerBit) == 0u) {
            return RadarRuntimeStatus::MalformedInput;
        }
        rolesMask |= speakerBit;
        if (role == AudioChannelRole::Lfe) {
            ++lfeCount;
            continue;
        }
        if (!IsDirectionalAudioChannelRole(role)) {
            return RadarRuntimeStatus::MalformedInput;
        }
        // AudioChannelLayout::IsValid intentionally enforces canonical
        // WAVEFORMATEXTENSIBLE ordering. Radar input may be explicitly
        // role-permuted (for channel diagnostics or mirrored test fixtures),
        // so derive azimuth from the validated role set instead of index.
        const std::optional<float> azimuth =
            RadarRoleAzimuthDegrees(layout.kind, role);
        if (!azimuth.has_value() || !std::isfinite(*azimuth)) {
            return RadarRuntimeStatus::MalformedInput;
        }
        const float radians = NormalizeDegrees(*azimuth) * kDegreesToRadians;
        float x = std::cos(radians);
        float y = std::sin(radians);
        if (std::fabs(x) < 1.0e-6f) x = 0.0f;
        if (std::fabs(y) < 1.0e-6f) y = 0.0f;
        validated.directional[channel] = true;
        validated.directionX[channel] = x;
        validated.directionY[channel] = y;
        ++validated.directionalChannelCount;
    }

    const uint32_t expectedDirectionalChannels = is51 ? 5u : 7u;
    if (rolesMask != layout.channelMask || lfeCount != 1u ||
        validated.directionalChannelCount != expectedDirectionalChannels) {
        return RadarRuntimeStatus::MalformedInput;
    }
    if (output != nullptr) {
        *output = validated;
    }
    return RadarRuntimeStatus::Active;
}

RadarRuntimeStatus ValidateBlock(const SurroundAudioBlockView& block,
                                 ValidatedLayout* layout) {
    if (block.layout.channelCount == 0u ||
        block.layout.channelCount > block.layout.roles.size() ||
        block.frameCount >
            std::numeric_limits<size_t>::max() / block.layout.channelCount ||
        block.firstSample >
            std::numeric_limits<uint64_t>::max() - block.frameCount) {
        return RadarRuntimeStatus::MalformedInput;
    }
    const size_t requiredSamples =
        block.frameCount * static_cast<size_t>(block.layout.channelCount);
    if (block.interleaved.size() < requiredSamples) {
        return RadarRuntimeStatus::MalformedInput;
    }
    const RadarRuntimeStatus layoutStatus = ValidateLayout(block.layout, layout);
    if (layoutStatus == RadarRuntimeStatus::MalformedInput) {
        return layoutStatus;
    }
    if (block.sampleRate != RadarProcessor::kSampleRate) {
        return RadarRuntimeStatus::UnsupportedSampleRate;
    }
    if (layoutStatus != RadarRuntimeStatus::Active) {
        return layoutStatus;
    }
    return RadarRuntimeStatus::Active;
}

float InterpolateCurve(
    const std::array<float, kRadarCustomCurvePointCount>& curve,
    float frequencyHz) {
    if (!std::isfinite(frequencyHz)) {
        return -60.0f;
    }
    if (frequencyHz <= kRadarCurveFrequenciesHz.front()) {
        return curve.front();
    }
    if (frequencyHz >= kRadarCurveFrequenciesHz.back()) {
        return curve.back();
    }
    const auto upper = std::upper_bound(
        kRadarCurveFrequenciesHz.begin(),
        kRadarCurveFrequenciesHz.end(),
        frequencyHz);
    const size_t upperIndex = static_cast<size_t>(
        std::distance(kRadarCurveFrequenciesHz.begin(), upper));
    const size_t lowerIndex = upperIndex - 1u;
    const float lowFrequency = kRadarCurveFrequenciesHz[lowerIndex];
    const float highFrequency = kRadarCurveFrequenciesHz[upperIndex];
    const float fraction =
        (std::log(frequencyHz) - std::log(lowFrequency)) /
        (std::log(highFrequency) - std::log(lowFrequency));
    return curve[lowerIndex] +
        (curve[upperIndex] - curve[lowerIndex]) * fraction;
}

float SmoothingCoefficient(uint32_t milliseconds) {
    if (milliseconds == 0u) {
        return 1.0f;
    }
    constexpr float hopMilliseconds =
        1000.0f * static_cast<float>(RadarProcessor::kHopSize) /
        static_cast<float>(RadarProcessor::kSampleRate);
    return 1.0f - std::exp(-hopMilliseconds / static_cast<float>(milliseconds));
}

struct RawRadarAnalysis {
    std::array<float, kRadarSectorCount> sectorPower{};
    float directionalEnergy{0.0f};
    float totalEnergy{0.0f};
    float confidence{0.0f};
};

} // namespace

struct RadarProcessor::Impl {
    explicit Impl(const RadarProcessorConfig& requestedConfig)
        : config(RadarProcessor::SanitizeConfig(requestedConfig)) {
        fftConfig = kiss_fftr_alloc(static_cast<int>(kFftSize), 0, nullptr, nullptr);
        if (fftConfig == nullptr) {
            throw std::runtime_error("Failed to allocate radar KissFFT plan");
        }

        float windowSquareSum = 0.0f;
        for (uint32_t index = 0; index < kFftSize; ++index) {
            const float phase = 2.0f * kPi * static_cast<float>(index) /
                                static_cast<float>(kFftSize - 1u);
            hannWindow[index] = 0.5f * (1.0f - std::cos(phase));
            windowSquareSum += hannWindow[index] * hannWindow[index];
        }
        interiorBinPowerScale =
            4.0f / (static_cast<float>(kFftSize) * windowSquareSum);
        edgeBinPowerScale = interiorBinPowerScale * 0.5f;
        RefreshBinWeights();
        ClearSignalState(0);
    }

    ~Impl() {
        if (fftConfig != nullptr) {
            kiss_fftr_free(fftConfig);
            fftConfig = nullptr;
        }
    }

    RadarProcessorConfig config;
    kiss_fftr_cfg fftConfig{nullptr};
    std::array<float, kFftSize> hannWindow{};
    std::array<float, kFftSize> fftInput{};
    std::array<kiss_fft_cpx, kBinCount> fftOutput{};
    std::array<std::array<float, kBinCount>, kMaximumChannels> channelPower{};
    std::array<float, kBinCount> binPowerWeights{};
    float interiorBinPowerScale{0.0f};
    float edgeBinPowerScale{0.0f};

    std::array<std::array<float, kFftSize>, kMaximumChannels> sampleRing{};
    size_t ringWritePosition{0};
    uint64_t receivedFrames{0};
    uint64_t nextWindowAtFrame{kFftSize};
    uint64_t expectedNextSample{0};
    bool haveStream{false};
    AudioChannelLayout layout{};
    ValidatedLayout validatedLayout{};
    uint64_t streamGeneration{0};

    std::array<float, kRadarSectorCount> smoothedPower{};
    std::array<uint32_t, kRadarSectorCount> holdSamplesRemaining{};
    float displayConfidence{0.0f};

    std::array<RadarFrame, kFrameQueueCapacity> readyFrames{};
    size_t readyReadPosition{0};
    size_t readyWritePosition{0};
    size_t readyCount{0};
    RadarRuntimeStatus status{RadarRuntimeStatus::WaitingForAudio};

    void RefreshBinWeights() {
        for (uint32_t bin = 0; bin < kBinCount; ++bin) {
            const float frequency = static_cast<float>(bin) *
                                    static_cast<float>(kSampleRate) /
                                    static_cast<float>(kFftSize);
            const float gainDb = RadarProcessor::FrequencyWeightDb(
                config.preset, frequency, config.customCurveDb);
            binPowerWeights[bin] = std::pow(10.0f, gainDb / 10.0f);
        }
        // Directional DC energy is not meaningful and otherwise lets device
        // offsets dominate the radar.
        binPowerWeights[0] = 0.0f;
    }

    void ClearSignalState(uint64_t generation) {
        for (auto& channel : sampleRing) channel.fill(0.0f);
        for (auto& channel : channelPower) channel.fill(0.0f);
        ringWritePosition = 0;
        receivedFrames = 0;
        nextWindowAtFrame = kFftSize;
        expectedNextSample = 0;
        haveStream = false;
        layout = {};
        validatedLayout = {};
        streamGeneration = generation;
        smoothedPower.fill(0.0f);
        holdSamplesRemaining.fill(0u);
        displayConfidence = 0.0f;
        readyReadPosition = 0;
        readyWritePosition = 0;
        readyCount = 0;
        status = RadarRuntimeStatus::WaitingForAudio;
    }

    void BeginStream(const SurroundAudioBlockView& block,
                     const ValidatedLayout& validated) {
        layout = block.layout;
        validatedLayout = validated;
        streamGeneration = block.streamGeneration;
        expectedNextSample = block.firstSample;
        haveStream = true;
    }

    void Enqueue(RadarFrame frame) {
        if (readyCount == readyFrames.size()) {
            readyReadPosition = (readyReadPosition + 1u) % readyFrames.size();
            --readyCount;
        }
        readyFrames[readyWritePosition] = std::move(frame);
        readyWritePosition = (readyWritePosition + 1u) % readyFrames.size();
        ++readyCount;
    }

    RadarFrame StatusFrame(const SurroundAudioBlockView& block,
                           RadarRuntimeStatus frameStatus) const {
        RadarFrame frame;
        frame.timestampSample = block.firstSample;
        frame.timestampSeconds = static_cast<double>(block.firstSample) /
                                 static_cast<double>(block.sampleRate == 0u
                                     ? kSampleRate : block.sampleRate);
        frame.streamGeneration = block.streamGeneration;
        frame.sampleRate = block.sampleRate;
        frame.layout = block.layout;
        frame.preset = config.preset;
        frame.sectorActivitiesDbfs.fill(kRadarSilenceDbfs);
        frame.status = frameStatus;
        return frame;
    }

    void ReportInvalidBlock(const SurroundAudioBlockView& block,
                            RadarRuntimeStatus blockStatus) {
        ClearSignalState(block.streamGeneration);
        status = blockStatus;
        Enqueue(StatusFrame(block, blockStatus));
    }

    RawRadarAnalysis ComputeRawWindow() {
        for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
            if (!validatedLayout.directional[channel]) {
                channelPower[channel].fill(0.0f);
                continue;
            }
            for (uint32_t sample = 0; sample < kFftSize; ++sample) {
                const size_t source = (ringWritePosition + sample) % kFftSize;
                const float value = sampleRing[channel][source];
                fftInput[sample] = (std::isfinite(value) ? value : 0.0f) *
                                   hannWindow[sample];
            }
            kiss_fftr(fftConfig,
                      reinterpret_cast<const kiss_fft_scalar*>(fftInput.data()),
                      fftOutput.data());
            for (uint32_t bin = 0; bin < kBinCount; ++bin) {
                const float real = fftOutput[bin].r;
                const float imaginary = fftOutput[bin].i;
                const float scale = (bin == 0u || bin + 1u == kBinCount)
                    ? edgeBinPowerScale : interiorBinPowerScale;
                channelPower[channel][bin] =
                    (real * real + imaginary * imaginary) * scale;
            }
        }

        RawRadarAnalysis analysis;
        for (uint32_t bin = 1; bin < kBinCount; ++bin) {
            const float spectralWeight = binPowerWeights[bin];
            if (!(spectralWeight > 0.0f) || !std::isfinite(spectralWeight)) {
                continue;
            }
            float totalPower = 0.0f;
            float vectorX = 0.0f;
            float vectorY = 0.0f;
            for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
                if (!validatedLayout.directional[channel]) continue;
                const float power = channelPower[channel][bin];
                totalPower += power;
                vectorX += power * validatedLayout.directionX[channel];
                vectorY += power * validatedLayout.directionY[channel];
            }
            if (!(totalPower > kMinimumPower)) {
                continue;
            }

            const float vectorMagnitude = std::hypot(vectorX, vectorY);
            const float weightedTotal = totalPower * spectralWeight;
            analysis.totalEnergy += weightedTotal;
            if (!(vectorMagnitude > totalPower * kMinimumDirectionalRatio)) {
                continue;
            }

            const float weightedDirectional = vectorMagnitude * spectralWeight;
            analysis.directionalEnergy += weightedDirectional;
            const float azimuth = NormalizeDegrees(
                std::atan2(vectorY, vectorX) * kRadiansToDegrees);
            const float sectorPosition = azimuth / kRadarSectorWidthDegrees;
            const uint32_t lowerSector =
                static_cast<uint32_t>(std::floor(sectorPosition)) %
                static_cast<uint32_t>(kRadarSectorCount);
            const uint32_t upperSector =
                (lowerSector + 1u) % static_cast<uint32_t>(kRadarSectorCount);
            const float upperFraction = sectorPosition - std::floor(sectorPosition);
            analysis.sectorPower[lowerSector] +=
                weightedDirectional * (1.0f - upperFraction);
            analysis.sectorPower[upperSector] +=
                weightedDirectional * upperFraction;
        }
        if (analysis.totalEnergy > kMinimumPower) {
            analysis.confidence = std::clamp(
                analysis.directionalEnergy / analysis.totalEnergy, 0.0f, 1.0f);
        }
        return analysis;
    }

    void ProduceFrame(uint64_t windowStartSample) {
        const RawRadarAnalysis raw = ComputeRawWindow();
        const float attack = SmoothingCoefficient(config.attackMilliseconds);
        const float release = SmoothingCoefficient(config.releaseMilliseconds);
        const uint32_t configuredHoldSamples = static_cast<uint32_t>(
            std::min<uint64_t>(
                static_cast<uint64_t>(config.holdMilliseconds) * kSampleRate / 1000u,
                std::numeric_limits<uint32_t>::max()));

        bool anyHeld = false;
        for (size_t sector = 0; sector < kRadarSectorCount; ++sector) {
            const float target = std::max(raw.sectorPower[sector], 0.0f);
            float& current = smoothedPower[sector];
            if (target >= current) {
                current += (target - current) * attack;
                if (target > kMinimumPower) {
                    holdSamplesRemaining[sector] = configuredHoldSamples;
                }
            } else if (holdSamplesRemaining[sector] > 0u) {
                holdSamplesRemaining[sector] =
                    holdSamplesRemaining[sector] > kHopSize
                    ? holdSamplesRemaining[sector] - kHopSize : 0u;
                anyHeld = true;
            } else {
                current += (target - current) * release;
            }
            if (!std::isfinite(current) || current < 0.0f) current = 0.0f;
            anyHeld = anyHeld || holdSamplesRemaining[sector] > 0u;
        }

        if (raw.directionalEnergy > kMinimumPower) {
            displayConfidence = raw.confidence;
        } else if (!anyHeld) {
            displayConfidence += (0.0f - displayConfidence) * release;
        }

        RadarFrame frame;
        frame.timestampSample = windowStartSample + kFftSize / 2u;
        frame.timestampSeconds = static_cast<double>(frame.timestampSample) /
                                 static_cast<double>(kSampleRate);
        frame.streamGeneration = streamGeneration;
        frame.sampleRate = kSampleRate;
        frame.layout = layout;
        frame.preset = config.preset;

        float strongestDbfs = kRadarSilenceDbfs;
        for (uint32_t sector = 0; sector < kRadarSectorCount; ++sector) {
            const float dbfs = PowerToDbfs(smoothedPower[sector]);
            frame.sectorActivitiesDbfs[sector] = dbfs;
            if (dbfs > strongestDbfs) {
                strongestDbfs = dbfs;
                frame.strongestSector = sector;
            }
        }
        frame.strongestAzimuthDegrees =
            static_cast<float>(frame.strongestSector) * kRadarSectorWidthDegrees;
        if (strongestDbfs >= config.sensitivityDbfs &&
            displayConfidence > 0.0f) {
            frame.status = RadarRuntimeStatus::Active;
            frame.confidence = std::clamp(displayConfidence, 0.0f, 1.0f);
        } else {
            frame.status = RadarRuntimeStatus::Silent;
            frame.confidence = 0.0f;
        }
        status = frame.status;
        if (config.mode != RadarMode::Events) {
            Enqueue(std::move(frame));
        }
    }

    void PushAudio(const SurroundAudioBlockView& block) {
        ValidatedLayout blockLayout;
        const RadarRuntimeStatus blockStatus = ValidateBlock(block, &blockLayout);
        if (blockStatus != RadarRuntimeStatus::Active) {
            ReportInvalidBlock(block, blockStatus);
            return;
        }

        const bool streamChanged = !haveStream || block.discontinuity ||
            block.streamGeneration != streamGeneration ||
            !LayoutsEqual(block.layout, layout) ||
            block.firstSample != expectedNextSample;
        if (streamChanged) {
            ClearSignalState(block.streamGeneration);
            BeginStream(block, blockLayout);
        }
        if (block.frameCount == 0u) {
            expectedNextSample = block.firstSample;
            return;
        }

        const uint32_t channelCount = block.layout.channelCount;
        for (size_t frame = 0; frame < block.frameCount; ++frame) {
            const size_t sourceBase = frame * static_cast<size_t>(channelCount);
            for (uint32_t channel = 0; channel < channelCount; ++channel) {
                const float value = block.interleaved[sourceBase + channel];
                sampleRing[channel][ringWritePosition] =
                    std::isfinite(value) ? value : 0.0f;
            }
            ringWritePosition = (ringWritePosition + 1u) % kFftSize;
            ++receivedFrames;
            if (receivedFrames == nextWindowAtFrame) {
                const uint64_t endSampleExclusive =
                    block.firstSample + static_cast<uint64_t>(frame) + 1u;
                ProduceFrame(endSampleExclusive - kFftSize);
                nextWindowAtFrame += kHopSize;
            }
        }
        expectedNextSample = block.firstSample + static_cast<uint64_t>(block.frameCount);
    }

    RadarEventResult AnalyzeEventWindow(const SoundEvent& event,
                                        const SurroundAudioBlockView& window) {
        RadarEventResult result;
        result.recognitionEvent = event;
        result.preset = config.preset;

        ValidatedLayout eventLayout;
        const RadarRuntimeStatus blockStatus = ValidateBlock(window, &eventLayout);
        if (blockStatus != RadarRuntimeStatus::Active) {
            result.status = blockStatus;
            return result;
        }
        if (window.frameCount < kFftSize) {
            result.status = RadarRuntimeStatus::MalformedInput;
            return result;
        }

        layout = window.layout;
        validatedLayout = eventLayout;
        std::array<float, kRadarSectorCount> accumulatedPower{};
        float accumulatedDirectional = 0.0f;
        float accumulatedTotal = 0.0f;
        uint32_t windowCount = 0;
        for (size_t offset = 0;
             offset + kFftSize <= window.frameCount;
             offset += kHopSize) {
            for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
                for (uint32_t sample = 0; sample < kFftSize; ++sample) {
                    const size_t source =
                        (offset + sample) * static_cast<size_t>(layout.channelCount) +
                        channel;
                    const float value = window.interleaved[source];
                    sampleRing[channel][sample] =
                        std::isfinite(value) ? value : 0.0f;
                }
            }
            ringWritePosition = 0;
            const RawRadarAnalysis raw = ComputeRawWindow();
            for (size_t sector = 0; sector < kRadarSectorCount; ++sector) {
                accumulatedPower[sector] += raw.sectorPower[sector];
            }
            accumulatedDirectional += raw.directionalEnergy;
            accumulatedTotal += raw.totalEnergy;
            ++windowCount;
        }

        std::array<float, kRadarSectorCount> activitiesDbfs{};
        const float inverseWindowCount = 1.0f / static_cast<float>(windowCount);
        for (size_t sector = 0; sector < kRadarSectorCount; ++sector) {
            activitiesDbfs[sector] =
                PowerToDbfs(accumulatedPower[sector] * inverseWindowCount);
        }
        const float confidence = accumulatedTotal > kMinimumPower
            ? std::clamp(accumulatedDirectional / accumulatedTotal, 0.0f, 1.0f)
            : 0.0f;
        result.peakCount = RadarProcessor::SelectPeaks(
            activitiesDbfs,
            confidence,
            result.peaks,
            config.sensitivityDbfs);
        result.status = result.peakCount > 0u
            ? RadarRuntimeStatus::Active : RadarRuntimeStatus::Silent;
        return result;
    }
};

RadarProcessor::RadarProcessor()
    : RadarProcessor(RadarProcessorConfig{}) {}

RadarProcessor::RadarProcessor(const RadarProcessorConfig& config)
    : m_impl(std::make_unique<Impl>(config)) {}

RadarProcessor::~RadarProcessor() = default;
RadarProcessor::RadarProcessor(RadarProcessor&&) noexcept = default;
RadarProcessor& RadarProcessor::operator=(RadarProcessor&&) noexcept = default;

void RadarProcessor::SetConfig(const RadarProcessorConfig& config) {
    const uint64_t generation = m_impl->streamGeneration;
    m_impl->config = SanitizeConfig(config);
    m_impl->RefreshBinWeights();
    m_impl->ClearSignalState(generation);
}

const RadarProcessorConfig& RadarProcessor::GetConfig() const {
    return m_impl->config;
}

void RadarProcessor::Reset(uint64_t streamGeneration) {
    m_impl->ClearSignalState(streamGeneration);
}

void RadarProcessor::OnStreamReset(uint64_t streamGeneration,
                                   const AudioChannelLayout&) {
    Reset(streamGeneration);
}

void RadarProcessor::PushAudio(const SurroundAudioBlockView& block) {
    m_impl->PushAudio(block);
}

size_t RadarProcessor::GetAvailableFrames() const {
    return m_impl->readyCount;
}

bool RadarProcessor::PopFrame(RadarFrame& output) {
    if (m_impl->readyCount == 0u) {
        return false;
    }
    output = std::move(m_impl->readyFrames[m_impl->readyReadPosition]);
    m_impl->readyReadPosition =
        (m_impl->readyReadPosition + 1u) % m_impl->readyFrames.size();
    --m_impl->readyCount;
    return true;
}

RadarRuntimeStatus RadarProcessor::GetStatus() const {
    return m_impl->status;
}

RadarEventResult RadarProcessor::AnalyzeEventWindow(
    const SoundEvent& event,
    const SurroundAudioBlockView& window) const {
    // Event analysis owns a separate FFT plan and scratch space so it cannot
    // perturb the continuous stream's timeline, smoothing, or ready queue.
    Impl eventAnalyzer(m_impl->config);
    return eventAnalyzer.AnalyzeEventWindow(event, window);
}

uint32_t RadarProcessor::SelectPeaks(
    const std::array<float, kRadarSectorCount>& activitiesDbfs,
    float frameConfidence,
    std::array<RadarPeak, kRadarMaximumPeaks>& output,
    float sensitivityDbfs) {
    output = {};
    for (auto& peak : output) {
        peak.energyDbfs = kRadarSilenceDbfs;
        peak.angularUncertaintyDegrees = 180.0f;
    }

    frameConfidence = ClampFinite(frameConfidence, 0.0f, 1.0f, 0.0f);
    sensitivityDbfs = ClampFinite(sensitivityDbfs,
                                  kRadarSilenceDbfs,
                                  0.0f,
                                  -48.0f);
    if (!(frameConfidence > 0.0f)) {
        return 0u;
    }

    std::array<float, kRadarSectorCount> levels{};
    float strongest = kRadarSilenceDbfs;
    for (size_t sector = 0; sector < kRadarSectorCount; ++sector) {
        levels[sector] = ClampFinite(activitiesDbfs[sector],
                                     kRadarSilenceDbfs,
                                     0.0f,
                                     kRadarSilenceDbfs);
        strongest = std::max(strongest, levels[sector]);
    }
    if (strongest < sensitivityDbfs) {
        return 0u;
    }

    std::array<uint32_t, kRadarSectorCount> candidates{};
    size_t candidateCount = 0;
    for (uint32_t sector = 0; sector < kRadarSectorCount; ++sector) {
        const uint32_t previous =
            (sector + static_cast<uint32_t>(kRadarSectorCount) - 1u) %
            static_cast<uint32_t>(kRadarSectorCount);
        const uint32_t next =
            (sector + 1u) % static_cast<uint32_t>(kRadarSectorCount);
        const float level = levels[sector];
        if (level >= levels[previous] && level >= levels[next] &&
            level >= strongest - 18.0f && level >= sensitivityDbfs) {
            candidates[candidateCount++] = sector;
        }
    }
    std::stable_sort(candidates.begin(), candidates.begin() + candidateCount,
        [&levels](uint32_t left, uint32_t right) {
            return levels[left] > levels[right];
        });

    uint32_t outputCount = 0;
    for (size_t candidateIndex = 0;
         candidateIndex < candidateCount && outputCount < output.size();
         ++candidateIndex) {
        const uint32_t sector = candidates[candidateIndex];
        const float sectorAzimuth =
            static_cast<float>(sector) * kRadarSectorWidthDegrees;
        const uint32_t previous =
            (sector + static_cast<uint32_t>(kRadarSectorCount) - 1u) %
            static_cast<uint32_t>(kRadarSectorCount);
        const uint32_t next =
            (sector + 1u) % static_cast<uint32_t>(kRadarSectorCount);
        float directionX = 0.0f;
        float directionY = 0.0f;
        for (const uint32_t neighbor : {previous, sector, next}) {
            const float power = DbfsToPower(levels[neighbor]);
            const float radians = static_cast<float>(neighbor) *
                                  kRadarSectorWidthDegrees * kDegreesToRadians;
            directionX += power * std::cos(radians);
            directionY += power * std::sin(radians);
        }
        const float refinedAzimuth =
            std::hypot(directionX, directionY) > kMinimumPower
            ? NormalizeDegrees(std::atan2(directionY, directionX) * kRadiansToDegrees)
            : sectorAzimuth;

        bool separated = true;
        for (uint32_t existing = 0; existing < outputCount; ++existing) {
            if (CircularDistanceDegrees(
                    refinedAzimuth, output[existing].azimuthDegrees) < 30.0f) {
                separated = false;
                break;
            }
        }
        if (!separated) continue;

        const float relativeAmplitude = std::pow(
            10.0f, (levels[sector] - strongest) / 20.0f);
        const float confidence = std::clamp(
            frameConfidence * relativeAmplitude, 0.0f, 1.0f);

        RadarPeak& peak = output[outputCount];
        peak.azimuthDegrees = refinedAzimuth;
        peak.energyDbfs = levels[sector];
        peak.confidence = confidence;
        peak.angularUncertaintyDegrees = std::clamp(
            7.5f + (1.0f - confidence) * 82.5f, 7.5f, 90.0f);
        ++outputCount;
    }
    return outputCount;
}

float RadarProcessor::FrequencyWeightDb(
    RadarPreset preset,
    float frequencyHz,
    const std::array<float, kRadarCustomCurvePointCount>& customCurveDb) {
    switch (preset) {
    case RadarPreset::All:
        return 0.0f;
    case RadarPreset::Footsteps:
        return InterpolateCurve(kFootstepCurveDb, frequencyHz);
    case RadarPreset::Gunshots:
        return InterpolateCurve(kGunshotCurveDb, frequencyHz);
    case RadarPreset::Custom:
        return InterpolateCurve(customCurveDb, frequencyHz);
    }
    return 0.0f;
}

RadarProcessorConfig RadarProcessor::SanitizeConfig(RadarProcessorConfig config) {
    config.sensitivityDbfs = ClampFinite(
        config.sensitivityDbfs, -96.0f, 0.0f, -48.0f);
    config.attackMilliseconds = std::min(config.attackMilliseconds, 2000u);
    config.releaseMilliseconds = std::min(config.releaseMilliseconds, 5000u);
    config.holdMilliseconds = std::min(config.holdMilliseconds, 2000u);
    for (float& gainDb : config.customCurveDb) {
        gainDb = ClampFinite(gainDb, -24.0f, 24.0f, 0.0f);
    }
    return config;
}

} // namespace EchoRadar
