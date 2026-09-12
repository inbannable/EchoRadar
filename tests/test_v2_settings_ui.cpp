#include <gtest/gtest.h>

#include <settings/AppSettings.h>
#include <ui/AppState.h>

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>

using namespace EchoRadar;

namespace {

std::filesystem::path V2TestRoot(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string ReadV2Text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

} // namespace

TEST(V2AppSettings, DefaultsAndPathUseSchemaFourNamespace) {
    const AppSettings settings;
    EXPECT_EQ(settings.schemaVersion, 4u);
    EXPECT_EQ(settings.radar.mode, RadarMode::Continuous);
    EXPECT_EQ(settings.radar.preset, RadarPreset::All);
    EXPECT_FLOAT_EQ(settings.radar.sensitivityDbfs, -48.0f);
    EXPECT_EQ(settings.radar.attackMilliseconds, 30u);
    EXPECT_EQ(settings.radar.releaseMilliseconds, 250u);
    EXPECT_EQ(settings.radar.holdMilliseconds, 300u);
    EXPECT_EQ(settings.radar.customCurveDb.size(), 16u);

    const auto path = AppSettingsFile::DefaultPath();
    EXPECT_EQ(path.filename(), "settings.json");
    EXPECT_EQ(path.parent_path().filename(), "v2");
    EXPECT_FALSE(path.parent_path().parent_path().empty());
}

TEST(V2AppSettings, MigratesSchemaThreeIntoV2Defaults) {
    const auto root = V2TestRoot("echoradar-settings-v3-migration-test");
    const auto path = root / "settings.json";
    {
        std::ofstream output(path);
        output << R"({"schema_version":3,"audio_profile_name":"Legacy",)"
                  R"("direction_footsteps_enabled":false,)"
                  R"("direction_gunshots_enabled":true,)"
                  R"("overlay_visibility":"always","overlay_radius_px":220,)"
                  R"("overlay_opacity":0.6,"overlay_offset_x":45})";
    }

    AppSettings loaded;
    std::string error;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.schemaVersion, 4u);
    EXPECT_EQ(loaded.audioProfile.name, "Legacy");
    EXPECT_FALSE(loaded.direction.enableFootsteps);
    EXPECT_TRUE(loaded.direction.enableGunshots);
    EXPECT_EQ(loaded.radar.mode, RadarMode::Continuous);
    ASSERT_EQ(loaded.hudDisplays.size(), 1u);
    EXPECT_EQ(loaded.hudDisplays.front().displayId, "default");
    EXPECT_FLOAT_EQ(loaded.hudDisplays.front().scale, 2.0f);
    EXPECT_FLOAT_EQ(loaded.hudDisplays.front().opacity, 0.6f);
    EXPECT_FLOAT_EQ(loaded.hudDisplays.front().offsetX, 45.0f);

    ASSERT_TRUE(AppSettingsFile::Save(path, loaded, &error)) << error;
    EXPECT_NE(
        ReadV2Text(path).find("\"schema_version\": 4"),
        std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(V2AppSettings, RoundTripPreservesV2DashboardRadarOnboardingAndHud) {
    const auto root = V2TestRoot("echoradar-settings-v4-roundtrip-test");
    const auto path = root / "settings.json";

    AppSettings source;
    source.radar.mode = RadarMode::Combined;
    source.radar.preset = RadarPreset::Custom;
    source.radar.sensitivityDbfs = -36.5f;
    source.radar.holdMilliseconds = 1800;
    source.radar.customCurveDb[7] = 4.25f;
    source.dashboard.audioSetupCollapsed = true;
    source.dashboard.windowX = -640.0f;
    source.dashboard.windowWidth = 1440.0f;
    source.onboarding.endpointSelected = true;
    source.onboarding.step = SetupStep::ValidateFormat;
    source.onboarding.supportState =
        SetupSupportState::UnsupportedSampleRate;
    source.eventDisplay.showSuppressedEvents = true;
    source.eventDisplay.maximumRecentEvents = 42;

    HudDisplaySettings second;
    second.scale = 1.4f;
    second.opacity = 0.7f;
    second.anchor = HudAnchor::BottomRight;
    second.offsetX = -55.0f;
    second.showDegreeLabels = true;
    second.persistenceSeconds = 3.5f;
    second.previewBackground = HudPreviewBackground::Checkerboard;
    second.editMode = true;
    second.strongestColor = {0.8f, 0.2f, 0.1f, 0.9f};
    ASSERT_TRUE(UpdateHudDisplaySettings(source, "display-2", second));

    std::string error;
    ASSERT_TRUE(AppSettingsFile::Save(path, source, &error)) << error;
    AppSettings loaded;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;

    EXPECT_EQ(loaded.radar.mode, RadarMode::Combined);
    EXPECT_EQ(loaded.radar.preset, RadarPreset::Custom);
    EXPECT_FLOAT_EQ(loaded.radar.sensitivityDbfs, -36.5f);
    EXPECT_EQ(loaded.radar.holdMilliseconds, 1800u);
    EXPECT_FLOAT_EQ(loaded.radar.customCurveDb[7], 4.25f);
    EXPECT_TRUE(loaded.dashboard.audioSetupCollapsed);
    EXPECT_FLOAT_EQ(loaded.dashboard.windowX, -640.0f);
    EXPECT_EQ(
        loaded.onboarding.supportState,
        SetupSupportState::UnsupportedSampleRate);
    EXPECT_TRUE(loaded.eventDisplay.showSuppressedEvents);
    EXPECT_EQ(loaded.eventDisplay.maximumRecentEvents, 42u);
    const auto* hud = FindHudDisplaySettings(loaded, "display-2");
    ASSERT_NE(hud, nullptr);
    EXPECT_EQ(hud->anchor, HudAnchor::BottomRight);
    EXPECT_FLOAT_EQ(hud->scale, 1.4f);
    EXPECT_FLOAT_EQ(hud->strongestColor.alpha, 0.9f);
    EXPECT_TRUE(hud->showDegreeLabels);
    EXPECT_TRUE(hud->editMode);
    std::filesystem::remove_all(root);
}

TEST(V2AppSettings, ClampsRadarDashboardEventsAndHud) {
    AppSettings settings;
    settings.radar.sensitivityDbfs =
        std::numeric_limits<float>::quiet_NaN();
    settings.radar.attackMilliseconds = 9000;
    settings.radar.releaseMilliseconds = 9000;
    settings.radar.holdMilliseconds = 9000;
    settings.radar.customCurveDb[0] = -100.0f;
    settings.radar.customCurveDb[1] =
        std::numeric_limits<float>::infinity();
    settings.dashboard.windowWidth = 100.0f;
    settings.dashboard.windowHeight = 100.0f;
    settings.eventDisplay.maximumRecentEvents = 9000;
    settings.eventDisplay.minimumConfidence = -2.0f;
    settings.hudDisplays.front().scale = 99.0f;
    settings.hudDisplays.front().opacity =
        std::numeric_limits<float>::quiet_NaN();
    settings.hudDisplays.front().sectorColor.red = -1.0f;

    settings = AppSettings::Clamp(std::move(settings));
    EXPECT_FLOAT_EQ(settings.radar.sensitivityDbfs, -48.0f);
    EXPECT_EQ(settings.radar.attackMilliseconds, 2000u);
    EXPECT_EQ(settings.radar.releaseMilliseconds, 5000u);
    EXPECT_EQ(settings.radar.holdMilliseconds, 2000u);
    EXPECT_FLOAT_EQ(settings.radar.customCurveDb[0], -24.0f);
    EXPECT_FLOAT_EQ(settings.radar.customCurveDb[1], 0.0f);
    EXPECT_FLOAT_EQ(settings.dashboard.windowWidth, 1100.0f);
    EXPECT_FLOAT_EQ(settings.dashboard.windowHeight, 700.0f);
    EXPECT_EQ(settings.eventDisplay.maximumRecentEvents, 500u);
    EXPECT_FLOAT_EQ(settings.eventDisplay.minimumConfidence, 0.0f);
    EXPECT_FLOAT_EQ(settings.hudDisplays.front().scale, 4.0f);
    EXPECT_FLOAT_EQ(settings.hudDisplays.front().opacity, 0.9f);
    EXPECT_FLOAT_EQ(settings.hudDisplays.front().sectorColor.red, 0.0f);
}

TEST(V2AppSettings, PerDisplayCollectionIsBoundedAndUpdatesInPlace) {
    AppSettings settings;
    for (size_t index = 1;
         index < AppSettings::kMaximumHudDisplays; ++index) {
        HudDisplaySettings hud;
        hud.opacity = static_cast<float>(index) / 20.0f;
        ASSERT_TRUE(UpdateHudDisplaySettings(
            settings, "display-" + std::to_string(index), hud));
    }
    EXPECT_EQ(
        settings.hudDisplays.size(),
        AppSettings::kMaximumHudDisplays);

    HudDisplaySettings replacement;
    replacement.scale = 1.75f;
    EXPECT_TRUE(UpdateHudDisplaySettings(
        settings, "display-3", replacement));
    ASSERT_NE(FindHudDisplaySettings(settings, "display-3"), nullptr);
    EXPECT_FLOAT_EQ(
        FindHudDisplaySettings(settings, "display-3")->scale, 1.75f);
    EXPECT_FALSE(UpdateHudDisplaySettings(
        settings, "overflow-display", HudDisplaySettings{}));
    EXPECT_EQ(FindHudDisplaySettings(settings, "missing"), nullptr);
}

TEST(V2AppSettings, DebouncedSaverFlushesLatestGenerationOnShutdown) {
    const auto root = V2TestRoot("echoradar-debounced-settings-save-test");
    const auto path = root / "settings.json";
    auto store = std::make_shared<RuntimeSettingsStore>(path);
    {
        DebouncedSettingsSaver saver(
            store, std::chrono::seconds(30), std::chrono::seconds(60));
        AppSettings settings = store->Snapshot();
        settings.radar.sensitivityDbfs = -60.0f;
        ASSERT_TRUE(store->Update(settings, false));
        saver.RequestSave();

        settings.radar.sensitivityDbfs = -36.0f;
        settings.hudDisplays.front().opacity = 0.42f;
        ASSERT_TRUE(store->Update(settings, false));
        saver.RequestSave();
        // Destruction must bypass the long debounce and persist the latest
        // generation before the settings store can go away.
    }

    AppSettings loaded;
    std::string error;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_FLOAT_EQ(loaded.radar.sensitivityDbfs, -36.0f);
    ASSERT_FALSE(loaded.hudDisplays.empty());
    EXPECT_FLOAT_EQ(loaded.hudDisplays.front().opacity, 0.42f);
    std::filesystem::remove_all(root);
}

TEST(V2AppSettings, HudBoundsStayInsideNegativeOriginWorkArea) {
    const HudRect workArea{-1920.0f, 40.0f, 1920.0f, 1040.0f};
    const HudRect clamped = ClampHudToUsableMonitorBounds(
        {-2200.0f, -100.0f, 300.0f, 260.0f}, workArea);
    EXPECT_FLOAT_EQ(clamped.x, -1920.0f);
    EXPECT_FLOAT_EQ(clamped.y, 40.0f);
    EXPECT_LE(clamped.x + clamped.width, 0.0f);
    EXPECT_LE(clamped.y + clamped.height, 1080.0f);

    HudDisplaySettings settings;
    settings.anchor = HudAnchor::BottomRight;
    settings.offsetX = 5000.0f;
    settings.offsetY = 5000.0f;
    settings.scale = 2.0f;
    const HudRect resolved =
        ResolveHudBounds(settings, workArea, 200.0f, 150.0f);
    EXPECT_FLOAT_EQ(resolved.x + resolved.width, 0.0f);
    EXPECT_FLOAT_EQ(resolved.y + resolved.height, 1080.0f);

    const HudRect oversized = ClampHudToUsableMonitorBounds(
        {-5000.0f, -5000.0f, 9000.0f, 9000.0f}, workArea);
    EXPECT_FLOAT_EQ(oversized.x, workArea.x);
    EXPECT_FLOAT_EQ(oversized.y, workArea.y);
    EXPECT_FLOAT_EQ(oversized.width, workArea.width);
    EXPECT_FLOAT_EQ(oversized.height, workArea.height);
}

TEST(V2UiState, PublishedSnapshotsAreImmutableAndDetachedFromProducer) {
    LatestSnapshotPublisher publisher;
    AppSnapshot source;
    source.revision = 7;
    source.capture.endpointName = "Surround endpoint";
    source.settings.radar.holdMilliseconds = 777;
    source.outputDevices.push_back(AudioDeviceInfo{});
    publisher.Publish(source);

    source.revision = 99;
    source.capture.endpointName = "mutated";
    source.settings.radar.holdMilliseconds = 1;
    source.outputDevices.clear();

    const auto snapshot = publisher.Latest();
    static_assert(std::is_const_v<
        typename decltype(snapshot)::element_type>);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->revision, 7u);
    EXPECT_EQ(snapshot->capture.endpointName, "Surround endpoint");
    EXPECT_EQ(snapshot->settings.radar.holdMilliseconds, 777u);
    EXPECT_EQ(snapshot->outputDevices.size(), 1u);
}

TEST(V2UiState, CommandQueueIsTypedFifoAndThreadSafe) {
    UiCommandQueue queue;
    queue.Push(SetRadarModeCommand{RadarMode::Events});
    queue.Push(SetActiveHudDisplayCommand{
        "display-2", {-1920.0f, 0.0f, 1920.0f, 1040.0f}});
    queue.Push(SetRadarSmoothingCommand{10, 20, 30});
    queue.Push(SetDashboardSectionCollapsedCommand{
        DashboardSection::Advanced, false});

    UiCommand command;
    ASSERT_TRUE(queue.TryPop(command));
    ASSERT_TRUE(std::holds_alternative<SetRadarModeCommand>(command));
    EXPECT_EQ(
        std::get<SetRadarModeCommand>(command).mode,
        RadarMode::Events);
    const auto remaining = queue.Drain();
    ASSERT_EQ(remaining.size(), 3u);
    ASSERT_TRUE(
        std::holds_alternative<SetActiveHudDisplayCommand>(remaining[0]));
    EXPECT_EQ(
        std::get<SetActiveHudDisplayCommand>(remaining[0]).displayId,
        "display-2");
    EXPECT_FLOAT_EQ(
        std::get<SetActiveHudDisplayCommand>(remaining[0])
            .usableBounds.x,
        -1920.0f);
    EXPECT_TRUE(
        std::holds_alternative<SetRadarSmoothingCommand>(remaining[1]));
    EXPECT_TRUE(
        std::holds_alternative<SetDashboardSectionCollapsedCommand>(
            remaining[2]));

    constexpr int kProducerCount = 4;
    constexpr int kCommandsPerProducer = 50;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&queue] {
            for (int index = 0; index < kCommandsPerProducer; ++index) {
                queue.Push(SetRecordingCommand{(index % 2) == 0});
            }
        });
    }
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(
        queue.Drain().size(),
        static_cast<size_t>(kProducerCount * kCommandsPerProducer));
    EXPECT_TRUE(queue.Empty());
}

TEST(V2Setup, CompletesSupportedFlowAndCanResumeBackward) {
    OnboardingSettings state;
    auto transition =
        ApplySetupEvent(state, SetupEvent::EndpointSelected);
    EXPECT_EQ(transition.status, SetupTransitionStatus::Applied);
    EXPECT_EQ(transition.state.step, SetupStep::ValidateFormat);

    transition =
        ApplySetupEvent(transition.state, SetupEvent::FormatSupported);
    EXPECT_EQ(transition.state.step, SetupStep::ConfirmChannels);
    transition = ApplySetupEvent(
        transition.state, SetupEvent::ChannelActivityConfirmed);
    EXPECT_EQ(transition.state.step, SetupStep::PreviewHud);
    transition = ApplySetupEvent(
        transition.state, SetupEvent::HudPreviewConfirmed);
    EXPECT_EQ(transition.state.step, SetupStep::Complete);
    EXPECT_TRUE(transition.state.completed);

    transition = ApplySetupEvent(transition.state, SetupEvent::Back);
    EXPECT_EQ(transition.state.step, SetupStep::PreviewHud);
    EXPECT_FALSE(transition.state.completed);
    EXPECT_FALSE(transition.state.hudPreviewed);
}

TEST(V2Setup, UnsupportedFormatsDisableOnlyDirectionalRadar) {
    auto state =
        ApplySetupEvent({}, SetupEvent::EndpointSelected).state;
    const auto unsupported = ApplySetupEvent(
        state, SetupEvent::FormatUnsupportedStereo);
    EXPECT_EQ(
        unsupported.status, SetupTransitionStatus::Unsupported);
    EXPECT_EQ(
        unsupported.state.supportState,
        SetupSupportState::UnsupportedStereo);
    EXPECT_EQ(unsupported.state.step, SetupStep::ValidateFormat);
    EXPECT_FALSE(unsupported.state.formatValidated);

    const auto blocked = ApplySetupEvent(
        unsupported.state, SetupEvent::ChannelActivityConfirmed);
    EXPECT_EQ(blocked.status, SetupTransitionStatus::Blocked);

    const auto availability =
        SetupAvailability(SetupSupportState::UnsupportedStereo);
    EXPECT_FALSE(availability.directionalRadar);
    EXPECT_TRUE(availability.recognitionAccessible);
    EXPECT_TRUE(availability.settingsAccessible);
    EXPECT_TRUE(availability.shouldOfferSoundSettings);
    EXPECT_FALSE(availability.explanation.empty());

    const auto sampleRate =
        SetupAvailability(SetupSupportState::UnsupportedSampleRate);
    EXPECT_FALSE(sampleRate.directionalRadar);
    EXPECT_TRUE(sampleRate.recognitionAccessible);
    EXPECT_TRUE(sampleRate.settingsAccessible);
}

TEST(V2Setup, HeadphonesCompleteSetupWithoutClaimingSurroundAndPersist) {
    auto state = ApplySetupEvent({}, SetupEvent::EndpointSelected).state;
    state = ApplySetupEvent(state, SetupEvent::HeadphoneFormatSupported).state;
    EXPECT_EQ(state.step, SetupStep::ConfirmChannels);
    EXPECT_FALSE(SetupAvailability(state.supportState).directionalRadar);
    state = ApplySetupEvent(state, SetupEvent::ChannelActivityConfirmed).state;
    state = ApplySetupEvent(state, SetupEvent::HudPreviewConfirmed).state;
    ASSERT_TRUE(state.completed);
    const auto root = V2TestRoot("echoradar-headphone-setup-test");
    const auto path = root / "settings.json";
    AppSettings settings;
    settings.onboarding = state;
    std::string error;
    ASSERT_TRUE(AppSettingsFile::Save(path, settings, &error)) << error;
    AppSettings loaded;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_TRUE(loaded.onboarding.completed);
    EXPECT_EQ(loaded.onboarding.supportState, SetupSupportState::HeadphoneStereo);
    EXPECT_FALSE(SetupAvailability(loaded.onboarding.supportState).directionalRadar);
    std::filesystem::remove_all(root);
}
