#include <recognition/PcmWav.h>

#include <phonon.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

using namespace EchoRadar;

static_assert(STEAMAUDIO_VERSION_MAJOR == 4 && STEAMAUDIO_VERSION_MINOR == 8 &&
              STEAMAUDIO_VERSION_PATCH == 1,
              "EchoRadar dataset rendering is pinned to Steam Audio v4.8.1");

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    float azimuth{0.0f};
    float elevation{0.0f};
    float distance{1.0f};
    float occlusion{0.0f};
    std::array<float, 3> transmission{1.0f, 1.0f, 1.0f};
    float directivity{1.0f};
    float reverbMix{0.0f};
};

bool ParseFloat(const char* text, float& value) {
    try {
        size_t used = 0;
        value = std::stof(text, &used);
        return used == std::string(text).size() && std::isfinite(value);
    } catch (...) {
        return false;
    }
}

bool ParseTransmission(const std::string& text, std::array<float, 3>& values) {
    std::istringstream stream(text);
    std::string part;
    for (float& value : values) {
        if (!std::getline(stream, part, ',') || !ParseFloat(part.c_str(), value)) return false;
    }
    return stream.rdbuf()->in_avail() == 0;
}

bool Parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--input" && index + 1 < argc) options.input = argv[++index];
        else if (argument == "--output" && index + 1 < argc) options.output = argv[++index];
        else if (argument == "--azimuth" && index + 1 < argc && ParseFloat(argv[index + 1], options.azimuth)) ++index;
        else if (argument == "--elevation" && index + 1 < argc && ParseFloat(argv[index + 1], options.elevation)) ++index;
        else if (argument == "--distance" && index + 1 < argc && ParseFloat(argv[index + 1], options.distance)) ++index;
        else if (argument == "--occlusion" && index + 1 < argc && ParseFloat(argv[index + 1], options.occlusion)) ++index;
        else if (argument == "--transmission" && index + 1 < argc && ParseTransmission(argv[index + 1], options.transmission)) ++index;
        else if (argument == "--directivity" && index + 1 < argc && ParseFloat(argv[index + 1], options.directivity)) ++index;
        else if (argument == "--reverb-mix" && index + 1 < argc && ParseFloat(argv[index + 1], options.reverbMix)) ++index;
        else return false;
    }
    const auto unit = [](float value) { return value >= 0.0f && value <= 1.0f; };
    return !options.input.empty() && !options.output.empty() && options.distance >= 1.0f &&
        unit(options.occlusion) && unit(options.directivity) && unit(options.reverbMix) &&
        std::all_of(options.transmission.begin(), options.transmission.end(), unit);
}

IPLVector3 Direction(float azimuthDegrees, float elevationDegrees) {
    const float azimuth = azimuthDegrees * std::numbers::pi_v<float> / 180.0f;
    const float elevation = elevationDegrees * std::numbers::pi_v<float> / 180.0f;
    const float horizontal = std::cos(elevation);
    return {std::sin(azimuth) * horizontal, std::sin(elevation), -std::cos(azimuth) * horizontal};
}

void ApplyShortReverb(std::vector<float>& stereo, float mix) {
    if (mix <= 0.0f) return;
    constexpr size_t kDelayLeft = 719;
    constexpr size_t kDelayRight = 1061;
    for (size_t frame = 0; frame < stereo.size() / 2; ++frame) {
        const float dryLeft = stereo[frame * 2];
        const float dryRight = stereo[frame * 2 + 1];
        const float wetLeft = frame >= kDelayLeft ? stereo[(frame - kDelayLeft) * 2] * 0.55f : 0.0f;
        const float wetRight = frame >= kDelayRight ? stereo[(frame - kDelayRight) * 2 + 1] * 0.50f : 0.0f;
        stereo[frame * 2] = dryLeft * (1.0f - mix) + wetLeft * mix;
        stereo[frame * 2 + 1] = dryRight * (1.0f - mix) + wetRight * mix;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "echoradar-steam-audio-renderer v4.8.1\n";
        return 0;
    }
    Options options;
    if (!Parse(argc, argv, options)) {
        std::cerr << "Usage: echoradar_steam_audio_renderer --input <mono-wav> --output <stereo-wav> "
                     "--azimuth <deg> --elevation <deg> --distance <m> --occlusion <0..1> "
                     "--transmission <low,mid,high> --directivity <0..1> --reverb-mix <0..1>\n";
        return 2;
    }

    std::string error;
    PcmAudio source;
    if (!LoadPcmWav(options.input, source, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    source = ResampleLinear(source, 48000);
    std::vector<float> mono(source.FrameCount());
    for (size_t frame = 0; frame < source.FrameCount(); ++frame) {
        const float left = source.interleaved[frame * source.channels];
        const float right = source.channels == 2 ? source.interleaved[frame * 2 + 1] : left;
        mono[frame] = 0.5f * (left + right);
    }

    IPLContextSettings contextSettings{};
    contextSettings.version = STEAMAUDIO_VERSION;
    IPLContext context{};
    if (iplContextCreate(&contextSettings, &context) != IPL_STATUS_SUCCESS) return 1;
    IPLAudioSettings audioSettings{48000, 1024};
    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
    IPLHRTF hrtf{};
    if (iplHRTFCreate(context, &audioSettings, &hrtfSettings, &hrtf) != IPL_STATUS_SUCCESS) return 1;

    IPLDirectEffectSettings directSettings{1};
    IPLDirectEffect direct{};
    iplDirectEffectCreate(context, &audioSettings, &directSettings, &direct);
    IPLBinauralEffectSettings binauralSettings{hrtf};
    IPLBinauralEffect binaural{};
    iplBinauralEffectCreate(context, &audioSettings, &binauralSettings, &binaural);

    IPLAudioBuffer inBuffer{};
    IPLAudioBuffer directBuffer{};
    IPLAudioBuffer outBuffer{};
    iplAudioBufferAllocate(context, 1, audioSettings.frameSize, &inBuffer);
    iplAudioBufferAllocate(context, 1, audioSettings.frameSize, &directBuffer);
    iplAudioBufferAllocate(context, 2, audioSettings.frameSize, &outBuffer);
    std::vector<float> interleaved(audioSettings.frameSize * 2);
    std::vector<float> rendered;
    rendered.reserve(mono.size() * 2);

    IPLDirectEffectParams directParams{};
    directParams.flags = static_cast<IPLDirectEffectFlags>(
        IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION |
        IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
        IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY |
        IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
        IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
    directParams.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    directParams.distanceAttenuation = 1.0f / options.distance;
    directParams.airAbsorption[0] = std::exp(-0.001f * options.distance);
    directParams.airAbsorption[1] = std::exp(-0.003f * options.distance);
    directParams.airAbsorption[2] = std::exp(-0.010f * options.distance);
    directParams.directivity = options.directivity;
    directParams.occlusion = 1.0f - options.occlusion;
    std::copy(options.transmission.begin(), options.transmission.end(), directParams.transmission);
    IPLBinauralEffectParams binauralParams{};
    binauralParams.direction = Direction(options.azimuth, options.elevation);
    binauralParams.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    binauralParams.spatialBlend = 1.0f;
    binauralParams.hrtf = hrtf;

    for (size_t start = 0; start < mono.size(); start += audioSettings.frameSize) {
        const size_t count = std::min<size_t>(audioSettings.frameSize, mono.size() - start);
        std::fill(inBuffer.data[0], inBuffer.data[0] + audioSettings.frameSize, 0.0f);
        std::copy_n(mono.data() + start, count, inBuffer.data[0]);
        iplDirectEffectApply(direct, &directParams, &inBuffer, &directBuffer);
        iplBinauralEffectApply(binaural, &binauralParams, &directBuffer, &outBuffer);
        iplAudioBufferInterleave(context, &outBuffer, interleaved.data());
        rendered.insert(rendered.end(), interleaved.begin(), interleaved.begin() + count * 2);
    }

    ApplyShortReverb(rendered, options.reverbMix);
    iplAudioBufferFree(context, &outBuffer);
    iplAudioBufferFree(context, &directBuffer);
    iplAudioBufferFree(context, &inBuffer);
    iplBinauralEffectRelease(&binaural);
    iplDirectEffectRelease(&direct);
    iplHRTFRelease(&hrtf);
    iplContextRelease(&context);

    PcmAudio output{48000, 2, std::move(rendered)};
    if (!WritePcm16Wav(options.output, output, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
