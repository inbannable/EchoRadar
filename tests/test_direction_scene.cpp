#include <localization/DirectionModelPackage.h>
#include <localization/DirectionOnnxEngine.h>
#include <localization/DirectionSceneCoordinator.h>

#include <dataset/AssetInventory.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace EchoRadar {
namespace {

DirectionModelPackage TestPackage() {
    DirectionModelPackage package;
    package.modelVersion = "test-direction";
    package.preprocessingVersion = "stereo-onset-v4-scene48";
    package.activityThresholds = {0.20f, 0.20f};
    package.duplicateMergeDegrees = 7.5f;
    package.uncertaintyCount = 3;
    package.uncertainty[0] = {0.0f, 60.0f};
    package.uncertainty[1] = {0.5f, 25.0f};
    package.uncertainty[2] = {1.0f, 8.0f};
    return package;
}

TEST(DirectionSceneCoordinatorTest, ClustersBoundaryOnceAndResetsOnDiscontinuity) {
    DirectionSceneCoordinator coordinator;
    V4SoundEvent first;
    first.onsetSample = 10'000;
    first.streamGeneration = 4;
    V4SoundEvent boundary = first;
    boundary.onsetSample += 120u * 48'000u / 1000u;
    V4SoundEvent next = boundary;
    ++next.onsetSample;

    const uint64_t firstScene = coordinator.AddEvent(11, first);
    EXPECT_EQ(coordinator.AddEvent(12, boundary), firstScene);
    const uint64_t nextScene = coordinator.AddEvent(13, next);
    EXPECT_NE(nextScene, firstScene);
    EXPECT_EQ(coordinator.PendingSceneCount(), 2u);

    const uint64_t firstStart = first.onsetSample - 40u * 48'000u / 1000u;
    EXPECT_TRUE(coordinator.TakeReady(firstStart + 12'303u, 0).empty());
    const auto ready = coordinator.TakeReady(firstStart + 12'304u, 0);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].events.size(), 2u);
    EXPECT_TRUE(ready[0].audioAvailable);
    EXPECT_EQ(ready[0].sceneStartSample, firstStart);

    coordinator.Reset(5);
    EXPECT_EQ(coordinator.PendingSceneCount(), 0u);
    EXPECT_EQ(coordinator.StreamGeneration(), 5u);
}

TEST(DirectionSceneCoordinatorTest, OutOfOrderEventMovesAnchorToEarliestOnset) {
    DirectionSceneCoordinator coordinator;
    V4SoundEvent later;
    later.onsetSample = 20'000;
    later.streamGeneration = 9;
    V4SoundEvent earlier = later;
    earlier.onsetSample -= 80u * 48'000u / 1000u;
    const uint64_t sceneId = coordinator.AddEvent(1, later);
    EXPECT_EQ(coordinator.AddEvent(2, earlier), sceneId);
    const uint64_t expectedStart = earlier.onsetSample - 40u * 48'000u / 1000u;
    const auto ready = coordinator.TakeReady(expectedStart + 12'304u, 0);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].anchorEventSample, earlier.onsetSample);
    EXPECT_EQ(ready[0].sceneStartSample, expectedStart);
    EXPECT_EQ(ready[0].events.size(), 2u);
    EXPECT_EQ(ready[0].events[0].event.onsetSample, earlier.onsetSample);
}

TEST(DirectionSceneCoordinatorTest, OutOfOrderEventCannotWidenGroupPastJoinWindow) {
    DirectionSceneCoordinator coordinator;
    V4SoundEvent middle;
    middle.onsetSample = 20'000;
    middle.streamGeneration = 10;
    V4SoundEvent later = middle;
    later.onsetSample += 100u * 48'000u / 1000u;
    V4SoundEvent earlier = middle;
    earlier.onsetSample -= 100u * 48'000u / 1000u;

    const uint64_t middleScene = coordinator.AddEvent(1, middle);
    EXPECT_EQ(coordinator.AddEvent(2, later), middleScene);
    const uint64_t earlierScene = coordinator.AddEvent(3, earlier);
    EXPECT_NE(earlierScene, middleScene);
    EXPECT_EQ(coordinator.PendingSceneCount(), 2u);

    const uint64_t earlierStart = earlier.onsetSample - 40u * 48'000u / 1000u;
    const auto firstReady = coordinator.TakeReady(earlierStart + 12'304u, 0);
    ASSERT_EQ(firstReady.size(), 1u);
    EXPECT_EQ(firstReady[0].sceneId, earlierScene);
    EXPECT_EQ(firstReady[0].events.size(), 1u);
}

TEST(DirectionOnnxEngineTest, PreservesFifteenDegreeSourcesAndFiltersGunshots) {
    DirectionModelPackage package = TestPackage();
    DirectionOnnxEngine::RawOutput output{};
    // Gunshot track points right.
    output[0] = 0.95f;
    // Two footstep tracks are exactly 15 degrees apart and must not merge.
    const float radians = 15.0f * 3.14159265358979323846f / 180.0f;
    output[9 + 2] = 0.90f;
    output[12] = std::sin(radians) * 0.85f;
    output[12 + 2] = std::cos(radians) * 0.85f;
    // A near-duplicate third footstep track must merge into the first.
    const float duplicate = 3.0f * 3.14159265358979323846f / 180.0f;
    output[15] = std::sin(duplicate) * 0.75f;
    output[15 + 2] = std::cos(duplicate) * 0.75f;

    const auto all = DirectionOnnxEngine::PostProcess(
        package, output,
        DirectionSceneResult::kGunshotClassBit | DirectionSceneResult::kFootstepClassBit);
    EXPECT_EQ(all.sourceCount, 3u);

    const auto footsteps = DirectionOnnxEngine::PostProcess(
        package, output, DirectionSceneResult::kFootstepClassBit);
    ASSERT_EQ(footsteps.sourceCount, 2u);
    EXPECT_NEAR(footsteps.sources[0].azimuthDegrees, 0.0f, 3.1f);
    EXPECT_NEAR(footsteps.sources[1].azimuthDegrees, 15.0f, 0.1f);
    EXPECT_LT(footsteps.sources[0].uncertaintyDegrees, 25.0f);
}

TEST(DirectionOnnxEngineTest, ExtractsExactlyFiveByFortyEightBySixtyFour) {
    DirectionOnnxEngine engine(TestPackage());
    std::vector<float> stereo(12'304u * 2u);
    for (size_t frame = 0; frame < 12'304u; ++frame) {
        const int leftCode = static_cast<int>((frame * 37u) % 101u) - 50;
        const int rightCode = static_cast<int>((frame * 53u + 17u) % 127u) - 63;
        stereo[frame * 2u] = static_cast<float>(leftCode) / 500.0f;
        stereo[frame * 2u + 1u] = static_cast<float>(rightCode) / 630.0f;
    }
    std::vector<float> features;
    std::string error;
    ASSERT_TRUE(engine.ExtractFeatures(stereo, features, &error)) << error;
    EXPECT_EQ(features.size(), 5u * 48u * 64u);
    EXPECT_TRUE(std::all_of(features.begin(), features.end(), [](float value) {
        return std::isfinite(value);
    }));

    // Generated by Python stereo_onset_features() from the exact integer-code
    // fixture above. Cover every feature plane at the beginning, middle, and
    // end of the 48-frame scene to lock the cross-language tensor contract.
    struct PythonReference {
        uint32_t frame;
        uint32_t plane;
        uint32_t mel;
        float value;
        float tolerance;
    };
    constexpr std::array<PythonReference, 20> references{{
        {0, 0, 10, 0.0000268221f, 0.00001f},
        {0, 1, 10, -100.0f, 0.05f},
        {0, 2, 10, 0.12974468f, 0.002f},
        {0, 3, 10, -0.13406485f, 0.002f},
        {0, 4, 10, -0.16396864f, 0.002f},
        {7, 0, 20, 0.14697850f, 0.002f},
        {7, 1, 20, -58.829727f, 0.05f},
        {7, 2, 20, 0.55316836f, 0.002f},
        {7, 3, 20, -0.31858733f, 0.002f},
        {7, 4, 20, -0.02112121f, 0.002f},
        {23, 0, 5, 0.02839565f, 0.002f},
        {23, 1, 5, -69.228600f, 0.05f},
        {23, 2, 5, -1.0f, 0.002f},
        {23, 3, 5, 0.78359079f, 0.002f},
        {23, 4, 5, -0.39952028f, 0.002f},
        {47, 0, 10, 0.0000287294f, 0.00001f},
        {47, 1, 10, -99.708023f, 0.05f},
        {47, 2, 10, 0.13114150f, 0.002f},
        {47, 3, 10, -0.20327306f, 0.002f},
        {47, 4, 10, -0.14884901f, 0.002f},
    }};
    for (const auto& reference : references) {
        const size_t index =
            (static_cast<size_t>(reference.plane) * 48u + reference.frame) * 64u +
            reference.mel;
        EXPECT_NEAR(features[index], reference.value, reference.tolerance)
            << "frame=" << reference.frame << " plane=" << reference.plane
            << " mel=" << reference.mel;
    }
}

TEST(DirectionModelPackageTest, ValidatesHashAndFlatContract) {
    const std::filesystem::path root =
        std::filesystem::path(::testing::TempDir()) / "echoradar-direction-package-test";
    std::filesystem::create_directories(root);
    const auto modelPath = root / "direction.onnx";
    {
        std::ofstream model(modelPath, std::ios::binary | std::ios::trunc);
        model << "test-onnx-payload";
    }
    bool hashOk = false;
    const std::string digest = AssetInventory::ComputeFileSha256(modelPath, &hashOk);
    ASSERT_TRUE(hashOk);
    {
        std::ofstream metadata(root / "direction.json", std::ios::binary | std::ios::trunc);
        metadata
            << "{\n"
            << "\"package_version\":1,\n"
            << "\"model_version\":\"test-v1\",\n"
            << "\"model_file\":\"direction.onnx\",\n"
            << "\"model_sha256\":\"" << digest << "\",\n"
            << "\"preprocessing_version\":\"stereo-onset-v4-scene48\",\n"
            << "\"sample_rate\":48000,\"fft_size\":1024,\"hop_size\":240,\n"
            << "\"mel_bins\":64,\"context_frames\":48,\"context_samples\":12304,\n"
            << "\"input_channels\":5,\"class_order\":\"gunshot,footstep\",\n"
            << "\"track_order\":\"exchangeable-0,exchangeable-1,exchangeable-2\",\n"
            << "\"track_count\":3,\"coordinate_system\":\"x-right,y-up,z-forward\",\n"
            << "\"maximum_sources\":3,\"elevation_min_degrees\":-60,\n"
            << "\"elevation_max_degrees\":60,\"threshold_gunshot\":0.4,\n"
            << "\"threshold_footstep\":0.35,\"duplicate_merge_degrees\":7.5,\n"
            << "\"minimum_training_separation_degrees\":15,\n"
            << "\"pcen_smoothing\":0.025,\"pcen_alpha\":0.98,\n"
            << "\"pcen_delta\":2,\"pcen_root\":0.5,\"pcen_epsilon\":0.000001,\n"
            << "\"uncertainty_count\":3,\n"
            << "\"uncertainty_confidence_0\":0,\"uncertainty_p90_degrees_0\":60,\n"
            << "\"uncertainty_confidence_1\":0.5,\"uncertainty_p90_degrees_1\":25,\n"
            << "\"uncertainty_confidence_2\":1,\"uncertainty_p90_degrees_2\":8\n"
            << "}\n";
    }
    DirectionModelPackage package;
    std::string error;
    ASSERT_TRUE(DirectionModelPackage::Load(root, package, &error)) << error;
    EXPECT_EQ(package.contextSamples, 12'304u);
    EXPECT_NEAR(package.UncertaintyDegrees(0.75f), 16.5f, 0.001f);

    {
        std::ofstream model(modelPath, std::ios::binary | std::ios::app);
        model << "tamper";
    }
    EXPECT_FALSE(DirectionModelPackage::Load(root, package, &error));
    EXPECT_NE(error.find("SHA-256"), std::string::npos);
}

} // namespace
} // namespace EchoRadar
