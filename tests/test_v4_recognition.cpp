#include <recognition/StereoOnsetFeatureExtractor.h>
#include <recognition/V4ModelPackage.h>
#include <recognition/V4ProbabilityModel.h>
#include <recognition/V4Recognizer.h>
#include <dataset/AssetInventory.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace EchoRadar;

namespace {

class CapturingV4Model final : public V4ProbabilityModel {
public:
    explicit CapturingV4Model(std::vector<V4ModelOutput> outputs = {})
        : m_outputs(std::move(outputs)) {}

    size_t InputFrames() const override { return 128; }
    size_t InputBins() const override { return 64; }
    size_t InputChannels() const override { return 5; }

    bool Predict(std::span<const float> features, V4ModelOutput& output,
                 std::string*) override {
        lastInput.assign(features.begin(), features.end());
        if (m_outputs.empty()) {
            output = {};
        } else {
            output = m_outputs[std::min(callCount, m_outputs.size() - 1)];
        }
        ++callCount;
        return true;
    }

    std::vector<float> lastInput;
    size_t callCount{0};

private:
    std::vector<V4ModelOutput> m_outputs;
};

V4ModelPackage TestPackage() {
    V4ModelPackage package;
    package.modelVersion = "test-v4";
    package.quietThresholds = {0.5f, 0.5f};
    package.busyThresholds = {0.7f, 0.7f};
    package.minimumSpacingSamples = {1680, 3840};
    package.onsetOffsetSamples = {500, 500};
    package.sceneActivityCutoff = 0.5f;
    package.selfSuppressionThreshold = 0.95f;
    package.peakLookaheadFrames = 2;
    package.pulseSamples = 2400;
    return package;
}

std::vector<float> DirectionalSine(size_t frames) {
    std::vector<float> samples(frames * 2);
    for (size_t index = 0; index < frames; ++index) {
        const float phase = 2.0f * 3.14159265358979323846f * 1300.0f *
            static_cast<float>(index) / 48000.0f;
        samples[index * 2] = 0.8f * std::sin(phase);
        samples[index * 2 + 1] = 0.35f * std::sin(phase - 0.7f);
    }
    return samples;
}

std::vector<float> PythonGoldenSignal(size_t frames) {
    constexpr double kPi = 3.14159265358979323846;
    std::vector<float> samples(frames * 2);
    for (size_t index = 0; index < frames; ++index) {
        const double position = static_cast<double>(index);
        samples[index * 2] = static_cast<float>(
            0.35 * std::sin(2.0 * kPi * 437.0 * position / 48000.0) +
            0.12 * std::cos(2.0 * kPi * 2111.0 * position / 48000.0));
        samples[index * 2 + 1] = static_cast<float>(
            0.27 * std::sin(2.0 * kPi * 437.0 * position / 48000.0 - 0.45) +
            0.18 * std::cos(2.0 * kPi * 3203.0 * position / 48000.0 + 0.2));
    }
    return samples;
}

V4ModelOutput Output(float gunshot, float footstep,
                     std::array<float, 3> gunSource = {0.01f, 0.98f, 0.01f}) {
    V4ModelOutput output;
    output.onsetProbabilities = {gunshot, footstep};
    output.sourceProbabilities[0] = gunSource;
    output.sourceProbabilities[1] = {0.01f, 0.98f, 0.01f};
    return output;
}

} // namespace

TEST(V4Features, StreamingChunksMatchWholeInput) {
    const size_t frameCount = 1024 + 6 * 240;
    const auto samples = DirectionalSine(frameCount);
    StereoOnsetFeatureExtractor whole;
    StereoOnsetFeatureExtractor chunked;
    whole.PushInterleaved(samples.data(), frameCount);
    size_t cursor = 0;
    for (const size_t take : std::array<size_t, 5>{137, 509, 83, 911, frameCount - 1640}) {
        chunked.PushInterleaved(samples.data() + cursor * 2, take);
        cursor += take;
    }
    StereoOnsetFeatureFrame expected;
    StereoOnsetFeatureFrame actual;
    size_t count = 0;
    while (whole.PopFrame(expected)) {
        ASSERT_TRUE(chunked.PopFrame(actual));
        EXPECT_EQ(actual.startSample, expected.startSample);
        EXPECT_NEAR(actual.sceneActivity, expected.sceneActivity, 1e-6f);
        for (size_t index = 0; index < expected.values.size(); ++index) {
            EXPECT_NEAR(actual.values[index], expected.values[index], 1e-6f);
        }
        ++count;
    }
    EXPECT_EQ(count, 7u);
    EXPECT_EQ(chunked.GetAvailableFrames(), 0u);
}

TEST(V4Features, ChannelSwapHasExpectedSpatialSymmetry) {
    const size_t frameCount = 1024 + 2 * 240;
    const auto samples = DirectionalSine(frameCount);
    std::vector<float> swapped(samples.size());
    for (size_t index = 0; index < frameCount; ++index) {
        swapped[index * 2] = samples[index * 2 + 1];
        swapped[index * 2 + 1] = samples[index * 2];
    }
    StereoOnsetFeatureExtractor originalExtractor;
    StereoOnsetFeatureExtractor swappedExtractor;
    originalExtractor.PushInterleaved(samples.data(), frameCount);
    swappedExtractor.PushInterleaved(swapped.data(), frameCount);
    StereoOnsetFeatureFrame original;
    StereoOnsetFeatureFrame reversed;
    while (originalExtractor.PopFrame(original)) {
        ASSERT_TRUE(swappedExtractor.PopFrame(reversed));
        for (size_t mel = 0; mel < 64; ++mel) {
            EXPECT_NEAR(original.values[mel], reversed.values[mel], 2e-5f);
            EXPECT_NEAR(original.values[64 + mel], reversed.values[64 + mel], 2e-4f);
            EXPECT_NEAR(original.values[128 + mel], -reversed.values[128 + mel], 2e-4f);
            EXPECT_NEAR(original.values[192 + mel], reversed.values[192 + mel], 2e-4f);
            EXPECT_NEAR(original.values[256 + mel], -reversed.values[256 + mel], 2e-4f);
        }
    }
}

TEST(V4Features, MatchesPythonStereoOnsetV4GoldenVector) {
    const auto samples = PythonGoldenSignal(1024 + 2 * 240);
    StereoOnsetFeatureExtractor extractor;
    extractor.PushInterleaved(samples.data(), samples.size() / 2);
    const std::array<size_t, 4> bins{0, 7, 19, 31};
    const std::array<std::array<float, 4>, 5> expectedFirst{{
        {0.000741839f, 0.288344860f, 0.000006318f, 0.047825336f},
        {-85.568878f, -23.147560f, -100.0f, -66.567322f},
        {0.134225294f, 0.075126775f, 0.006172016f, -1.0f},
        {0.882825077f, 0.900447547f, 0.170635089f, -0.065334909f},
        {0.380237043f, 0.434964597f, 0.085060395f, 0.373530269f},
    }};
    const std::array<std::array<float, 4>, 5> expectedLast{{
        {0.000395656f, 0.288363695f, 0.000007391f, 0.047788024f},
        {-88.299675f, -23.147243f, -100.0f, -66.570808f},
        {0.058254313f, 0.075137369f, 0.018559190f, -1.0f},
        {0.685808003f, 0.900461137f, 0.192464307f, -0.390415698f},
        {0.635967553f, 0.434936196f, 0.076165758f, 0.033339832f},
    }};

    StereoOnsetFeatureFrame frame;
    for (size_t frameIndex = 0; frameIndex < 3; ++frameIndex) {
        ASSERT_TRUE(extractor.PopFrame(frame));
        if (frameIndex == 1) continue;
        const auto& expected = frameIndex == 0 ? expectedFirst : expectedLast;
        for (size_t plane = 0; plane < 5; ++plane) {
            const float tolerance = plane == 1 ? 0.1f : (plane == 0 ? 2e-4f : 0.01f);
            for (size_t binIndex = 0; binIndex < bins.size(); ++binIndex) {
                EXPECT_NEAR(frame.values[plane * 64 + bins[binIndex]],
                            expected[plane][binIndex], tolerance)
                    << "frame=" << frameIndex << " plane=" << plane
                    << " mel=" << bins[binIndex];
            }
        }
    }
}

TEST(V4Recognizer, LeftPadsStartupContextAndInfersFirstFeature) {
    auto model = std::make_shared<CapturingV4Model>();
    V4Recognizer recognizer(model, TestPackage());
    const auto samples = DirectionalSine(1024);
    recognizer.PushInterleaved(samples.data(), 1024);
    ASSERT_EQ(model->callCount, 1u);
    ASSERT_EQ(model->lastInput.size(), 5u * 128u * 64u);
    for (size_t frame = 0; frame < 127; ++frame) {
        EXPECT_FLOAT_EQ(model->lastInput[(1 * 128 + frame) * 64], -100.0f);
        EXPECT_FLOAT_EQ(model->lastInput[(0 * 128 + frame) * 64], 0.0f);
    }
    EXPECT_GT(*std::max_element(model->lastInput.begin() + 127 * 64,
                                model->lastInput.begin() + 128 * 64), 0.0f);
}

TEST(V4Recognizer, AppliesPeakSpacingAndCalibratedOnset) {
    std::vector<V4ModelOutput> outputs(15, Output(0.1f, 0.1f));
    outputs[1] = Output(0.4f, 0.1f);
    outputs[2] = Output(0.9f, 0.1f);
    outputs[3] = Output(0.3f, 0.1f);
    auto model = std::make_shared<CapturingV4Model>(outputs);
    V4Recognizer recognizer(model, TestPackage());
    const size_t featureFrames = 29;
    const size_t pcmFrames = 1024 + (featureFrames - 1) * 240;
    std::vector<float> silence(pcmFrames * 2, 0.0f);
    const auto events = recognizer.PushInterleaved(silence.data(), pcmFrames);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].soundClass, SoundClass::Gunshot);
    EXPECT_EQ(events[0].onsetSample, 1984u - 500u);
    EXPECT_EQ(events[0].detectedSample, 2944u);
    EXPECT_FALSE(events[0].suppressed);
    EXPECT_EQ(events[0].sourceHint, SoundSourceHint::Remote);
}

TEST(V4Recognizer, AppliesRuntimeTuningOnNextAudioBlock) {
    std::vector<V4ModelOutput> outputs(15, Output(0.1f, 0.1f));
    outputs[1] = Output(0.4f, 0.1f);
    outputs[2] = Output(0.9f, 0.1f);
    auto model = std::make_shared<CapturingV4Model>(outputs);
    V4ModelPackage package = TestPackage();
    auto tuningStore = std::make_shared<V4RuntimeTuningStore>(
        V4RuntimeTuning::FromPackage(package));
    auto tuning = tuningStore->Snapshot();
    tuning.onsetOffsetSamples[0] = 480;
    tuningStore->Update(tuning);
    V4Recognizer recognizer(model, package, {}, tuningStore);
    const size_t pcmFrames = 1024 + 28 * 240;
    std::vector<float> silence(pcmFrames * 2, 0.0f);
    const auto events = recognizer.PushInterleaved(silence.data(), pcmFrames);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].onsetSample, 1984u - 480u);
}

TEST(V4Recognizer, SuppressesSelfEventsFromRealtimeCallback) {
    std::vector<V4ModelOutput> outputs(15, Output(0.1f, 0.1f));
    outputs[2] = Output(0.9f, 0.1f, {0.98f, 0.01f, 0.01f});
    auto model = std::make_shared<CapturingV4Model>(outputs);
    size_t callbackCount = 0;
    V4Recognizer recognizer(model, TestPackage(),
                            [&](const V4SoundEvent&) { ++callbackCount; });
    const size_t pcmFrames = 1024 + 28 * 240;
    std::vector<float> silence(pcmFrames * 2, 0.0f);
    const auto events = recognizer.PushInterleaved(silence.data(), pcmFrames);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].suppressed);
    EXPECT_EQ(events[0].sourceHint, SoundSourceHint::Self);
    EXPECT_EQ(callbackCount, 0u);
    EXPECT_EQ(recognizer.Stats().suppressedEventCount, 1u);
}

TEST(V4Recognizer, FlushSupportsZeroLookaheadAndCarriesStreamGeneration) {
    std::vector<V4ModelOutput> outputs{Output(0.9f, 0.1f), Output(0.1f, 0.1f)};
    auto model = std::make_shared<CapturingV4Model>(outputs);
    V4ModelPackage package = TestPackage();
    package.peakLookaheadFrames = 0;
    V4Recognizer recognizer(model, package);
    recognizer.OnStreamReset(42);
    std::vector<float> silence((1024 + 2 * 240) * 2, 0.0f);
    EXPECT_TRUE(recognizer.PushInterleaved(silence.data(), silence.size() / 2).empty());
    const auto events = recognizer.Flush();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].streamGeneration, 42u);
}

TEST(V4ModelPackage, ValidatesExportContractAndChecksum) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("echoradar_v4_package_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path modelPath = root / "recognizer.onnx";
    std::ofstream(modelPath, std::ios::binary) << "v4-onnx-fixture";
    bool hashOk = false;
    const std::string hash = AssetInventory::ComputeFileSha256(modelPath, &hashOk);
    ASSERT_TRUE(hashOk);
    std::ofstream metadata(root / "model.json");
    metadata
        << "{\n"
        << "\"package_version\":4,\"model_version\":\"test-v4\","
        << "\"model_file\":\"recognizer.onnx\",\"model_sha256\":\"" << hash << "\","
        << "\"preprocessing_version\":\"stereo-onset-v4\","
        << "\"sample_rate\":48000,\"fft_size\":1024,\"hop_size\":240,"
        << "\"mel_bins\":64,\"context_frames\":128,\"input_channels\":5,"
        << "\"inference_stride_frames\":2,\"class_order\":\"gunshot,footstep\","
        << "\"source_order\":\"self,remote,unknown\",\"event_mode\":\"onset-peak\","
        << "\"pulse_ms\":50,\"pcen_smoothing\":0.025,\"pcen_alpha\":0.98,"
        << "\"pcen_delta\":2.0,\"pcen_root\":0.5,\"pcen_epsilon\":0.000001,"
        << "\"scene_activity_cutoff\":0.4,\"self_suppression_threshold\":0.95,"
        << "\"peak_lookahead_frames\":2,"
        << "\"threshold_quiet_gunshot\":0.45,\"threshold_busy_gunshot\":0.55,"
        << "\"minimum_spacing_ms_gunshot\":35,\"onset_offset_samples_gunshot\":500,"
        << "\"threshold_quiet_footstep\":0.50,\"threshold_busy_footstep\":0.70,"
        << "\"minimum_spacing_ms_footstep\":80,\"onset_offset_samples_footstep\":600\n"
        << "}\n";
    metadata.close();

    V4ModelPackage package;
    std::string error;
    EXPECT_TRUE(V4ModelPackage::Load(root, package, &error)) << error;
    EXPECT_EQ(package.modelVersion, "test-v4");
    EXPECT_EQ(package.minimumSpacingSamples[0], 1680u);
    EXPECT_EQ(package.pulseSamples, 2400u);
    std::ofstream(modelPath, std::ios::app) << "corruption";
    EXPECT_FALSE(V4ModelPackage::Load(root, package, &error));
    EXPECT_NE(error.find("SHA-256"), std::string::npos);
    fs::remove_all(root);
}
