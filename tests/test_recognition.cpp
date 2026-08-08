#include <recognition/Evaluation.h>
#include <recognition/EventPostProcessor.h>
#include <recognition/LogMelExtractor.h>
#include <recognition/ModelPackage.h>
#include <recognition/PcmWav.h>
#include <recognition/ProbabilityModel.h>
#include <recognition/SoundRecognizer.h>
#include <dataset/AssetInventory.h>

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
using namespace EchoRadar;

namespace {

class ScriptedModel final : public ProbabilityModel {
public:
    explicit ScriptedModel(std::vector<std::array<float, kSoundClassCount>> outputs)
        : m_outputs(std::move(outputs)) {}
    size_t InputFrames() const override { return 96; }
    size_t InputBins() const override { return 64; }
    bool Predict(std::span<const float> input,
                 std::array<float, kSoundClassCount>& probabilities,
                 std::string*) override {
        if (input.size() != InputFrames() * InputBins()) return false;
        probabilities = m_outputs[std::min(m_callCount, m_outputs.size() - 1)];
        ++m_callCount;
        return true;
    }

private:
    std::vector<std::array<float, kSoundClassCount>> m_outputs;
    size_t m_callCount{0};
};

fs::path TempPath(const std::string& suffix) {
    return fs::temp_directory_path() /
        ("echoradar_recognition_" + std::to_string(reinterpret_cast<uintptr_t>(&suffix)) + suffix);
}

} // namespace

TEST(RecognitionPcm, Pcm16RoundTripAndStereoResample) {
    const fs::path path = TempPath(".wav");
    PcmAudio source;
    source.sampleRate = 24000;
    source.channels = 1;
    source.interleaved = {0.0f, 0.5f, -0.5f, 0.25f};
    std::string error;
    ASSERT_TRUE(WritePcm16Wav(path, source, &error)) << error;
    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    const PcmAudio converted = ConvertToStereo48k(loaded);
    EXPECT_EQ(converted.sampleRate, 48000u);
    EXPECT_EQ(converted.channels, 2u);
    EXPECT_EQ(converted.FrameCount(), 8u);
    for (size_t i = 0; i < converted.FrameCount(); ++i) {
        EXPECT_FLOAT_EQ(converted.interleaved[i * 2], converted.interleaved[i * 2 + 1]);
    }
    fs::remove(path);
}

TEST(RecognitionLogMel, MatchesPythonLogmelV1GoldenVector) {
    std::vector<float> pcm(1024 * 2, 0.0f);
    pcm[512 * 2] = 1.0f;
    pcm[512 * 2 + 1] = 1.0f;
    LogMelExtractor extractor;
    extractor.PushInterleaved(pcm.data(), 1024);
    LogMelFrame frame;
    ASSERT_TRUE(extractor.PopFrame(frame));
    const std::array<float, 64> expected = {
        0.008142021f, 0.008003202f, 0.008584660f, 0.009113154f, 0.009592494f, 0.010026237f,
        0.010417703f, 0.011102649f, 0.012738956f, 0.011727685f, 0.012226013f, 0.014910308f,
        0.012980988f, 0.016082970f, 0.015297676f, 0.016242780f, 0.017381793f, 0.018415894f,
        0.019352751f, 0.020199507f, 0.020962829f, 0.021648910f, 0.024212649f, 0.023846554f,
        0.025619175f, 0.027300054f, 0.028149258f, 0.030080302f, 0.031015595f, 0.032689914f,
        0.034250032f, 0.036384244f, 0.037968080f, 0.039901283f, 0.041453585f, 0.044094536f,
        0.045959473f, 0.048497740f, 0.050528746f, 0.053337883f, 0.055708695f, 0.058780145f,
        0.061325390f, 0.064593568f, 0.067544706f, 0.071087062f, 0.074363783f, 0.078157999f,
        0.081781983f, 0.086025469f, 0.089906648f, 0.094419122f, 0.098928787f, 0.103688650f,
        0.108716249f, 0.113897353f, 0.119324327f, 0.125085801f, 0.130959809f, 0.137189656f,
        0.143627748f, 0.150538817f, 0.157438874f, 0.164834753f,
    };
    ASSERT_EQ(frame.values.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(frame.values[i], expected[i], 2e-5f) << "mel bin " << i;
    }
}

TEST(RecognitionPostProcessor, EmitsOverlappingClassesAndSeparatesBursts) {
    EventPostProcessor::Config config;
    config.modelVersion = "test-v1";
    config.minOffFrames = {1, 1, 1};
    config.refractorySamples = {100, 100, 100};
    EventPostProcessor processor(config);
    EXPECT_TRUE(processor.Process({1000, {0.9f, 0.8f, 0.1f}}).empty());
    const auto overlapping = processor.Process({1100, {0.1f, 0.1f, 0.1f}});
    ASSERT_EQ(overlapping.size(), 2u);
    EXPECT_EQ(overlapping[0].soundClass, SoundClass::Gunshot);
    EXPECT_EQ(overlapping[1].soundClass, SoundClass::Footstep);
    EXPECT_TRUE(processor.Process({1250, {0.9f, 0.1f, 0.1f}}).empty());
    const auto secondBurst = processor.Process({1400, {0.1f, 0.1f, 0.1f}});
    ASSERT_EQ(secondBurst.size(), 1u);
    EXPECT_EQ(secondBurst[0].soundClass, SoundClass::Gunshot);
}

TEST(RecognitionPostProcessor, OnsetPulseEmitsImmediatelyAndRearmsIndependently) {
    EventPostProcessor::Config config;
    config.mode = EventPostProcessor::Mode::OnsetPulse;
    config.modelVersion = "onset-test";
    config.onThresholds = {0.6f, 0.6f, 0.6f};
    config.rearmThresholds = {0.3f, 0.3f, 0.3f};
    config.refractorySamples = {100, 100, 100};
    config.onsetOffsetSamples = {20, 30, 40};
    config.pulseSamples = 2400;
    EventPostProcessor processor(config);

    const auto first = processor.Process({1000, {0.9f, 0.8f, 0.1f}});
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].onsetSample, 980u);
    EXPECT_EQ(first[0].endSample - first[0].onsetSample, 2400u);
    EXPECT_EQ(first[0].detectedSample, 1000u);
    EXPECT_EQ(first[1].onsetSample, 970u);
    EXPECT_FALSE(processor.IsAmbient());

    EXPECT_TRUE(processor.Process({1050, {0.9f, 0.8f, 0.1f}}).empty());
    EXPECT_TRUE(processor.Process({1100, {0.1f, 0.1f, 0.1f}}).empty());
    const auto second = processor.Process({1200, {0.9f, 0.1f, 0.1f}});
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].soundClass, SoundClass::Gunshot);
    EXPECT_TRUE(processor.Flush(1200).empty());
}

TEST(RecognitionFeatures, StereoPcenPreservesPhaseAndStreamingBoundaries) {
    LogMelExtractor::Config config;
    config.mode = LogMelExtractor::Mode::StereoPcenV2;
    LogMelExtractor whole(config);
    LogMelExtractor chunked(config);
    LogMelExtractor swapped(config);
    const size_t frames = 1024 + 512 * 5;
    std::vector<float> stereo(frames * 2);
    for (size_t index = 0; index < frames; ++index) {
        const float value = std::sin(2.0f * 3.14159265358979323846f * 1400.0f *
                                     static_cast<float>(index) / 48000.0f);
        stereo[index * 2] = value;
        stereo[index * 2 + 1] = -value;
    }
    whole.PushInterleaved(stereo.data(), frames);
    std::vector<float> channelSwapped(stereo.size());
    for (size_t index = 0; index < frames; ++index) {
        channelSwapped[index * 2] = stereo[index * 2 + 1];
        channelSwapped[index * 2 + 1] = stereo[index * 2];
    }
    swapped.PushInterleaved(channelSwapped.data(), frames);
    size_t cursor = 0;
    const std::array<size_t, 4> chunks{137u, 509u, 83u, frames - 729u};
    for (const size_t take : chunks) {
        chunked.PushInterleaved(stereo.data() + cursor * 2, take);
        cursor += take;
    }
    LogMelFrame expected;
    LogMelFrame actual;
    LogMelFrame swappedFrame;
    size_t count = 0;
    while (whole.PopFrame(expected)) {
        ASSERT_TRUE(chunked.PopFrame(actual));
        ASSERT_TRUE(swapped.PopFrame(swappedFrame));
        ASSERT_EQ(expected.values.size(), 128u);
        ASSERT_EQ(actual.values.size(), expected.values.size());
        EXPECT_GT(*std::max_element(expected.values.begin(), expected.values.begin() + 64), 0.0f);
        for (size_t index = 0; index < expected.values.size(); ++index) {
            EXPECT_NEAR(actual.values[index], expected.values[index], 1e-6f);
            EXPECT_NEAR(swappedFrame.values[index], expected.values[index], 1e-6f);
        }
        if (count == 0) {
            const std::array<size_t, 13> bins{0, 8, 16, 20, 21, 22, 23, 24, 32, 40, 48, 56, 63};
            const std::array<float, 13> pythonPcen{
                0.000000477f, 0.000007272f, 0.003360033f, 0.302734613f, 0.286017418f,
                0.078882694f, 0.004513741f, 0.000731111f, 0.000000238f, 0.0f, 0.0f, 0.0f, 0.0f};
            const std::array<float, 13> pythonDb{
                -100.0f, -100.0f, -78.962250f, -11.613205f, -25.045544f, -63.676170f,
                -77.659912f, -85.631638f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f};
            for (size_t index = 0; index < bins.size(); ++index) {
                EXPECT_NEAR(expected.values[bins[index]], pythonPcen[index], 2e-4f);
                EXPECT_NEAR(expected.values[64 + bins[index]], pythonDb[index], 0.05f);
            }
        }
        ++count;
    }
    EXPECT_GT(count, 0u);
    EXPECT_EQ(chunked.GetAvailableFrames(), 0u);
}

TEST(RecognitionMode, AmbientUntilAConfirmedTargetBecomesActive) {
    EventPostProcessor::Config config;
    config.minOnFrames = {2, 2, 2};
    config.minOffFrames = {2, 2, 2};
    EventPostProcessor processor(config);

    EXPECT_TRUE(processor.IsAmbient());
    processor.Process({1000, {0.9f, 0.1f, 0.1f}});
    EXPECT_TRUE(processor.IsAmbient());
    processor.Process({1500, {0.9f, 0.1f, 0.1f}});
    EXPECT_FALSE(processor.IsAmbient());
    EXPECT_TRUE(processor.IsActive(SoundClass::Gunshot));
    EXPECT_FALSE(processor.IsActive(SoundClass::Footstep));

    processor.Process({2000, {0.1f, 0.1f, 0.1f}});
    EXPECT_FALSE(processor.IsAmbient());
    const auto events = processor.Process({2500, {0.1f, 0.1f, 0.1f}});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(processor.IsAmbient());
}

TEST(RecognitionRuntime, WavToProbabilityModelToSoundEvents) {
    auto model = std::make_shared<ScriptedModel>(std::vector<std::array<float, 3>>{
        {0.9f, 0.8f, 0.1f}, {0.9f, 0.8f, 0.1f}, {0.1f, 0.1f, 0.1f}, {0.1f, 0.1f, 0.1f},
    });
    SoundRecognizer::Config config;
    config.inferenceStrideFrames = 1;
    config.postProcessing.modelVersion = "scripted";
    SoundRecognizer recognizer(model, config);
    const size_t frames = 1024 + 512 * 102;
    std::vector<float> silence(frames * 2, 0.0f);
    const auto events = recognizer.PushInterleaved(silence.data(), frames);
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0].modelVersion, "scripted");
    EXPECT_GE(recognizer.Stats().inferenceCount, 4u);
    EXPECT_TRUE(recognizer.LastError().empty());
}

TEST(RecognitionEvaluation, MatchesEventsByClassAndOnsetTolerance) {
    const std::vector<LabeledSoundEvent> truth = {
        {SoundClass::Gunshot, 1000, 1200},
        {SoundClass::Footstep, 5000, 5400},
        {SoundClass::Mechanical, 9000, 9800},
    };
    const std::vector<SoundEvent> predictions = {
        {SoundClass::Gunshot, 1100, 1300, 0.9f, "v"},
        {SoundClass::Footstep, 20000, 20200, 0.8f, "v"},
    };
    const EvaluationReport report = EvaluateEvents(truth, predictions, 60.0, 500);
    EXPECT_EQ(report.classes[0].truePositives, 1u);
    EXPECT_EQ(report.classes[1].falsePositives, 1u);
    EXPECT_EQ(report.classes[1].falseNegatives, 1u);
    EXPECT_EQ(report.classes[2].falseNegatives, 1u);
    EXPECT_DOUBLE_EQ(report.classes[1].falseAlertsPerMinute, 1.0);
}

TEST(RecognitionModelPackage, ValidatesContractAndChecksum) {
    const fs::path root = TempPath("_model");
    fs::create_directories(root);
    const fs::path modelPath = root / "recognizer.onnx";
    std::ofstream(modelPath, std::ios::binary) << "dummy onnx bytes";
    bool hashOk = false;
    const std::string hash = AssetInventory::ComputeFileSha256(modelPath, &hashOk);
    ASSERT_TRUE(hashOk);
    std::ofstream metadata(root / "model.json");
    metadata << "{\n"
        "\"model_version\":\"test-v1\",\"model_file\":\"recognizer.onnx\","
        "\"model_sha256\":\"" << hash << "\",\"preprocessing_version\":\"logmel-v1\","
        "\"sample_rate\":48000,\"fft_size\":1024,\"hop_size\":512,\"mel_bins\":64,"
        "\"context_frames\":96,\"inference_stride_frames\":5,"
        "\"class_order\":\"gunshot,footstep,mechanical\","
        "\"threshold_gunshot\":0.45,\"threshold_footstep\":0.50,\"threshold_mechanical\":0.55,"
        "\"off_threshold_gunshot\":0.30,\"off_threshold_footstep\":0.35,\"off_threshold_mechanical\":0.40,"
        "\"min_on_frames_gunshot\":1,\"min_on_frames_footstep\":1,\"min_on_frames_mechanical\":1,"
        "\"min_off_frames_gunshot\":2,\"min_off_frames_footstep\":2,\"min_off_frames_mechanical\":2,"
        "\"refractory_ms_gunshot\":50,\"refractory_ms_footstep\":50,\"refractory_ms_mechanical\":100\n} \n";
    metadata.close();
    ModelPackage package;
    std::string error;
    EXPECT_TRUE(ModelPackage::Load(root, package, &error)) << error;
    EXPECT_EQ(package.modelVersion, "test-v1");
    EXPECT_FLOAT_EQ(package.postProcessing.onThresholds[0], 0.45f);
    std::ofstream(modelPath, std::ios::app) << "corruption";
    EXPECT_FALSE(ModelPackage::Load(root, package, &error));
    EXPECT_NE(error.find("SHA-256"), std::string::npos);
    fs::remove_all(root);
}

TEST(RecognitionModelPackage, LoadsStereoPcenOnsetPulseContract) {
    const fs::path root = TempPath("_model_v2");
    fs::create_directories(root);
    const fs::path modelPath = root / "recognizer.onnx";
    std::ofstream(modelPath, std::ios::binary) << "dummy onset onnx bytes";
    bool hashOk = false;
    const std::string hash = AssetInventory::ComputeFileSha256(modelPath, &hashOk);
    ASSERT_TRUE(hashOk);
    std::ofstream metadata(root / "model.json");
    metadata << "{\n"
        "\"model_version\":\"test-onset-v3\",\"model_file\":\"recognizer.onnx\"," 
        "\"model_sha256\":\"" << hash << "\",\"preprocessing_version\":\"stereo-pcen-v2\"," 
        "\"sample_rate\":48000,\"fft_size\":1024,\"hop_size\":512,\"mel_bins\":64," 
        "\"context_frames\":96,\"input_channels\":2,\"inference_stride_frames\":2," 
        "\"class_order\":\"gunshot,footstep,mechanical\",\"event_mode\":\"onset-pulse\"," 
        "\"pulse_ms\":50,\"pcen_smoothing\":0.025,\"pcen_alpha\":0.98," 
        "\"pcen_delta\":2.0,\"pcen_root\":0.5,\"pcen_epsilon\":0.000001," 
        "\"threshold_gunshot\":0.45,\"threshold_footstep\":0.50,\"threshold_mechanical\":0.55," 
        "\"off_threshold_gunshot\":0.225,\"off_threshold_footstep\":0.25,\"off_threshold_mechanical\":0.275," 
        "\"rearm_threshold_gunshot\":0.225,\"rearm_threshold_footstep\":0.25,\"rearm_threshold_mechanical\":0.275," 
        "\"onset_offset_samples_gunshot\":500,\"onset_offset_samples_footstep\":600,"
        "\"onset_offset_samples_mechanical\":700," 
        "\"min_on_frames_gunshot\":1,\"min_on_frames_footstep\":1,\"min_on_frames_mechanical\":1," 
        "\"min_off_frames_gunshot\":1,\"min_off_frames_footstep\":1,\"min_off_frames_mechanical\":1," 
        "\"refractory_ms_gunshot\":40,\"refractory_ms_footstep\":60,\"refractory_ms_mechanical\":80\n}\n";
    metadata.close();
    ModelPackage package;
    std::string error;
    EXPECT_TRUE(ModelPackage::Load(root, package, &error)) << error;
    EXPECT_EQ(package.inputChannels, 2u);
    EXPECT_EQ(package.inferenceStrideFrames, 2u);
    EXPECT_EQ(package.postProcessing.mode, EventPostProcessor::Mode::OnsetPulse);
    EXPECT_EQ(package.postProcessing.pulseSamples, 2400u);
    EXPECT_EQ(package.postProcessing.onsetOffsetSamples[1], 600u);
    EXPECT_FLOAT_EQ(package.pcenSmoothing, 0.025f);
    fs::remove_all(root);
}
