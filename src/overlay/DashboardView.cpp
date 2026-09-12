#include "OverlayRenderer.h"
#include "DashboardTheme.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace EchoRadar {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr ImVec4 kCyan{0.10f, 0.82f, 0.92f, 1.0f};
constexpr ImVec4 kAmber{1.0f, 0.66f, 0.16f, 1.0f};
constexpr ImVec4 kRed{1.0f, 0.28f, 0.25f, 1.0f};
constexpr ImVec4 kMuted{0.50f, 0.58f, 0.65f, 1.0f};

const char* CaptureStateText(AudioCaptureState state) {
    switch (state) {
    case AudioCaptureState::Stopped: return "STOPPED";
    case AudioCaptureState::Starting: return "STARTING";
    case AudioCaptureState::Running: return "CAPTURING";
    case AudioCaptureState::Recovering: return "RECOVERING";
    case AudioCaptureState::Failed: return "FAILED";
    case AudioCaptureState::Unsupported: return "UNSUPPORTED";
    }
    return "FAILED";
}

const char* ModelStateText(ModelUiState state) {
    switch (state) {
    case ModelUiState::Disabled: return "MODEL OFF";
    case ModelUiState::Loading: return "MODEL LOADING";
    case ModelUiState::Ready: return "MODEL READY";
    case ModelUiState::Missing: return "MODEL MISSING";
    case ModelUiState::Malformed: return "MODEL INVALID";
    case ModelUiState::Failed: return "MODEL FAILED";
    }
    return "MODEL OFF";
}

float DbIntensity(float db, float sensitivity) {
    if (!std::isfinite(db) || db <= kRadarSilenceDbfs + 0.1f) return 0.0f;
    const float denominator = std::max(1.0f, -sensitivity);
    return std::clamp((db - sensitivity) / denominator, 0.0f, 1.0f);
}

ImVec2 DirectionPoint(ImVec2 center, float radius, float azimuthDegrees) {
    const float radians = (azimuthDegrees - 90.0f) * kPi / 180.0f;
    return {center.x + std::cos(radians) * radius,
            center.y + std::sin(radians) * radius};
}

void DrawMiniRadar(ImDrawList* drawList, ImVec2 center, float radius,
                   const std::array<float, kRadarSectorCount>& sectors,
                   float sensitivity) {
    drawList->AddCircle(center, radius, IM_COL32(90, 109, 120, 180), 0, 1.0f);
    for (size_t index = 0; index < sectors.size(); ++index) {
        const float intensity = DbIntensity(sectors[index], sensitivity);
        if (intensity <= 0.01f) continue;
        const float azimuth = static_cast<float>(index) * 15.0f;
        const ImVec2 end = DirectionPoint(
            center, radius * (0.35f + 0.65f * intensity), azimuth);
        drawList->AddLine(center, end,
                          IM_COL32(30, 215, 235,
                                   static_cast<int>(80 + 175 * intensity)),
                          1.5f);
    }
}

const char* SetupStepText(SetupStep step) {
    switch (step) {
    case SetupStep::SelectEndpoint: return "1 / 5  Select playback endpoint";
    case SetupStep::ValidateFormat: return "2 / 5  Validate native format";
    case SetupStep::ConfirmChannels: return "3 / 5  Confirm channel activity";
    case SetupStep::PreviewHud: return "4 / 5  Preview and position HUD";
    case SetupStep::Complete: return "5 / 5  Setup complete";
    }
    return "Setup";
}


}
void ApplyDashboardTheme() {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 10.0f;
        style.ChildRounding = 16.0f;
        style.WindowPadding = ImVec2(18.0f, 18.0f);
        style.CellPadding = ImVec2(8.0f, 8.0f);
        style.FrameBorderSize = 0.0f;
        style.ScrollbarSize = 9.0f;
        style.FrameRounding = 8.0f;
        style.PopupRounding = 6.0f;
        style.GrabRounding = 5.0f;
        style.ScrollbarRounding = 8.0f;
        style.ItemSpacing = ImVec2(12.0f, 12.0f);
        style.FramePadding = ImVec2(9.0f, 6.0f);
        style.Colors[ImGuiCol_WindowBg] =
            ImVec4(0.035f, 0.043f, 0.070f, 1.0f);
        style.Colors[ImGuiCol_ChildBg] =
            ImVec4(0.060f, 0.075f, 0.110f, 1.0f);
        style.Colors[ImGuiCol_PopupBg] =
            ImVec4(0.045f, 0.055f, 0.065f, 0.98f);
        style.Colors[ImGuiCol_Border] =
            ImVec4(0.17f, 0.21f, 0.29f, 0.45f);
        style.Colors[ImGuiCol_FrameBg] =
            ImVec4(0.085f, 0.105f, 0.12f, 1.0f);
        style.Colors[ImGuiCol_FrameBgHovered] =
            ImVec4(0.11f, 0.18f, 0.20f, 1.0f);
        style.Colors[ImGuiCol_Button] =
            ImVec4(0.08f, 0.20f, 0.22f, 1.0f);
        style.Colors[ImGuiCol_ButtonHovered] =
            ImVec4(0.08f, 0.34f, 0.37f, 1.0f);
        style.Colors[ImGuiCol_Header] =
            ImVec4(0.07f, 0.22f, 0.24f, 1.0f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.98f, 1.0f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.53f, 0.60f, 0.72f, 1.0f);
        style.Colors[ImGuiCol_CheckMark] = kCyan;
        style.Colors[ImGuiCol_SliderGrab] = kCyan;
        style.Colors[ImGuiCol_PlotHistogram] = kCyan;
        style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.075f, 0.095f, 0.14f, 1);

}
void OverlayRenderer::DrawDashboard(const AppSnapshot& snapshot) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("EchoRadar v2", nullptr, flags);
    const float scale = m_appliedUiScale;
    const bool compact = ImGui::GetContentRegionAvail().x < 920.0f * scale;
    ImGui::BeginChild("Navigation", ImVec2(compact ? 150.0f * scale : 200.0f * scale, 0), false);
    ImGui::Spacing();
    ImGui::TextColored(kCyan, "ECHO / RADAR");
    ImGui::TextDisabled("Audio intelligence");
    ImGui::Dummy(ImVec2(0, 28.0f * scale));
    const char* pages[]{"Overview", "Audio & headphones", "Event insights", "HUD studio", "Diagnostics"};
    for (int page = 0; page < IM_ARRAYSIZE(pages); ++page) {
        ImGui::PushID(page);
        if (ImGui::Selectable(pages[page], m_page == page, 0,
                              ImVec2(0, 40.0f * scale))) m_page = page;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 24.0f * scale));
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", snapshot.capture.endpointName.empty()
        ? "Waiting for playback device" : snapshot.capture.endpointName.c_str());
    ImGui::TextColored(snapshot.capture.signal.active ? kCyan : kMuted,
        "%s", snapshot.capture.signal.active ? "Audio is flowing" : "Waiting for audio");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("PageContent", ImVec2(0, 0), false);
    ImGui::TextDisabled("WORKSPACE / %s", pages[m_page]);
    ImGui::SetWindowFontScale(1.65f);
    ImGui::TextUnformatted(pages[m_page]);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    DrawHeader(snapshot);
    ImGui::Spacing();
    DrawInlineStatus(snapshot);
    if (m_page == 0) {
        DrawRadarWorkspace(snapshot);
        DrawRecentEvents(snapshot);
    } else if (m_page == 1) {
        DrawAudioSetup(snapshot);
    } else if (m_page == 2) {
        DrawRecentEvents(snapshot);
        DrawEventDetails(snapshot);
    } else if (m_page == 3) {
        DrawHudEditor(snapshot);
        DrawLiveRadar(snapshot, 360.0f * scale);
    } else {
        DrawAdvanced(snapshot);
    }
    ImGui::EndChild();
    ImGui::End();
}

void OverlayRenderer::DrawHeader(const AppSnapshot& snapshot) {
    if (ImGui::BeginTable("PipelineCards", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::BeginChild("SignalCard", ImVec2(0, 124.0f * m_appliedUiScale), true);
        ImGui::TextDisabled("01 / CAPTURE");
        ImGui::TextColored(snapshot.capture.signal.active ? kCyan : kMuted,
            "%s", snapshot.capture.state == AudioCaptureState::Running
                ? (snapshot.capture.signal.active ? "Audio active" : "Listening")
                : CaptureStateText(snapshot.capture.state));
        ImGui::Text("%.1f dBFS", snapshot.capture.signal.rmsDbfs);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("RecognitionCard", ImVec2(0, 124.0f * m_appliedUiScale), true);
        ImGui::TextDisabled("02 / RECOGNITION");
        ImGui::TextColored(snapshot.model.state == ModelUiState::Ready ? kCyan : kMuted,
            "%s", ModelStateText(snapshot.model.state));
        ImGui::Text("%llu inferences", static_cast<unsigned long long>(snapshot.model.inferenceCount));
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("DirectionCard", ImVec2(0, 124.0f * m_appliedUiScale), true);
        ImGui::TextDisabled("03 / DIRECTION");
        ImGui::TextColored(snapshot.layout.directionalRadarAvailable ? kCyan : kAmber,
            "%s", snapshot.layout.directionalRadarAvailable ? "Surround azimuth" : "Azimuth unavailable");
        ImGui::Text("%u channels / %u Hz", snapshot.layout.detected.channelCount,
                    snapshot.capture.sampleRate);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawInlineStatus(const AppSnapshot& snapshot) {
    if (!snapshot.settings.onboarding.completed) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.09f, 0.035f, 1.0f));
        ImGui::BeginChild("SetupBanner", ImVec2(0.0f, 54.0f), true);
        ImGui::TextColored(kAmber, "[!] FIRST-RUN SETUP");
        ImGui::SameLine();
        ImGui::TextUnformatted(SetupStepText(snapshot.settings.onboarding.step));
        ImGui::TextDisabled("Progress is saved. Recognition and settings remain available while surround radar is unavailable.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (snapshot.error) {
        const ImVec4 color = snapshot.error.kind == InlineErrorKind::CaptureFailure ||
            snapshot.error.kind == InlineErrorKind::MalformedModel ? kRed : kAmber;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(color.x * 0.12f, color.y * 0.12f,
                                     color.z * 0.12f, 1.0f));
        ImGui::BeginChild("InlineStatus", ImVec2(0.0f, 116.0f * m_appliedUiScale), true);
        ImGui::PushTextWrapPos();
        ImGui::TextColored(color, "%s %s",
                           snapshot.error.kind == InlineErrorKind::Loading ? "~" : "!",
                           snapshot.error.message.c_str());
        ImGui::PopTextWrapPos();
        if (!snapshot.error.recoveryAction.empty()) {
            ImGui::TextWrapped("%s", snapshot.error.recoveryAction.c_str());
        }
        if (snapshot.error.recoverable && m_config.commands) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 130.0f);
            if (ImGui::Button("Retry capture")) {
                m_config.commands->Emplace<RetryCaptureCommand>();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void OverlayRenderer::DrawRadarWorkspace(const AppSnapshot& snapshot) {
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f,
                                    460.0f * m_appliedUiScale, 580.0f * m_appliedUiScale);
    if (ImGui::GetContentRegionAvail().x < 850.0f * m_appliedUiScale) {
        DrawLiveRadar(snapshot, height);
        DrawQuickControls(snapshot, height);
        return;
    }
    if (ImGui::BeginTable("LiveWorkspace", 2,
                          ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Radar", ImGuiTableColumnFlags_WidthStretch, 2.15f);
        ImGui::TableSetupColumn("Quick controls", ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveRadar(snapshot, height);
        ImGui::TableNextColumn();
        DrawQuickControls(snapshot, height);
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawLiveRadar(const AppSnapshot& snapshot, float height) {
    ImGui::BeginChild("LiveRadarPanel", ImVec2(0.0f, height), true);
    if (snapshot.layout.detected.kind == AudioChannelLayoutKind::Stereo) {
        ImGui::TextUnformatted("HEADPHONE MONITOR");
        ImGui::TextDisabled("Playback mix / independent of event classification");
        ImGui::Dummy(ImVec2(0, 35.0f * m_appliedUiScale));
        ImGui::SetWindowFontScale(2.0f);
        ImGui::Text("%.1f dBFS", snapshot.capture.signal.rmsDbfs);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::TextUnformatted("Left / Right energy balance");
        const float balance = snapshot.capture.signal.balance;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float barWidth = ImGui::GetContentRegionAvail().x;
        ImDrawList* meter = ImGui::GetWindowDrawList();
        meter->AddRectFilled(origin, ImVec2(origin.x + barWidth, origin.y + 18),
                             IM_COL32(29, 40, 58, 255), 9);
        const float middle = origin.x + barWidth * 0.5f;
        const float marker = origin.x + 9 + (barWidth - 18) * (balance + 1) * 0.5f;
        meter->AddLine(ImVec2(middle, origin.y + 9), ImVec2(marker, origin.y + 9),
                       IM_COL32(25, 195, 217, 255), 5);
        meter->AddLine(ImVec2(middle, origin.y + 3), ImVec2(middle, origin.y + 15),
                       IM_COL32(120, 140, 160, 255), 1);
        meter->AddCircleFilled(ImVec2(marker, origin.y + 9), 6, IM_COL32(140, 238, 246, 255));
        ImGui::Dummy(ImVec2(barWidth, 18));
        ImGui::TextColored(kCyan, "%s", !snapshot.capture.signal.active ? "Waiting for sound"
            : balance < -0.15f ? "More energy on the left"
            : balance > 0.15f ? "More energy on the right" : "Balanced energy");
        ImGui::Spacing();
        ImGui::TextWrapped("Front, rear and height cannot be determined from channel energy in a headphone mix. Spatial processing may be inside your game, Windows, or sound card.");
        ImGui::EndChild();
        return;
    }
    ImGui::TextUnformatted("LIVE DIRECTIONAL ENERGY");
    ImGui::SameLine();
    ImGui::TextDisabled("azimuth only / 15 deg sectors");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float radius = std::max(80.0f, std::min(available.x, available.y) * 0.41f);
    const ImVec2 center{ImGui::GetCursorScreenPos().x + available.x * 0.50f,
                        ImGui::GetCursorScreenPos().y + available.y * 0.50f};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 grid = IM_COL32(75, 92, 101, 180);
    const ImU32 gridFaint = IM_COL32(56, 68, 76, 135);
    drawList->AddCircle(center, radius, grid, 96, 1.5f);
    drawList->AddCircle(center, radius * 0.66f, gridFaint, 72, 1.0f);
    drawList->AddCircle(center, radius * 0.33f, gridFaint, 48, 1.0f);
    for (int degrees = 0; degrees < 360; degrees += 30) {
        drawList->AddLine(DirectionPoint(center, radius * 0.12f,
                                         static_cast<float>(degrees)),
                          DirectionPoint(center, radius, static_cast<float>(degrees)),
                          gridFaint, degrees % 90 == 0 ? 1.25f : 0.7f);
    }
    const auto label = [&](const char* text, float azimuth) {
        const ImVec2 point = DirectionPoint(center, radius + 18.0f, azimuth);
        const ImVec2 size = ImGui::CalcTextSize(text);
        drawList->AddText({point.x - size.x * 0.5f, point.y - size.y * 0.5f},
                          IM_COL32(190, 205, 211, 255), text);
    };
    label("FRONT 0 deg", 0.0f);
    label("R 90 deg", 90.0f);
    label("REAR 180 deg", 180.0f);
    label("L 270 deg", 270.0f);

    const float sensitivity = snapshot.settings.radar.sensitivityDbfs;
    const bool drawContinuous =
        snapshot.radar.mode != RadarMode::Events;
    if (snapshot.layout.directionalRadarAvailable && drawContinuous) {
        for (size_t index = 0;
             index < snapshot.radar.frame.sectorActivitiesDbfs.size(); ++index) {
            const float intensity = DbIntensity(
                snapshot.radar.frame.sectorActivitiesDbfs[index], sensitivity);
            if (intensity <= 0.001f) continue;
            const float azimuth = static_cast<float>(index) * 15.0f;
            const float start = (azimuth - 7.0f - 90.0f) * kPi / 180.0f;
            const float finish = (azimuth + 7.0f - 90.0f) * kPi / 180.0f;
            const float activeRadius = radius * (0.24f + 0.76f * intensity);
            drawList->PathLineTo(center);
            drawList->PathArcTo(center, activeRadius, start, finish, 7);
            drawList->PathFillConvex(IM_COL32(
                18, 204, 224, static_cast<int>(35 + 170 * intensity)));
        }
    }

    std::optional<float> newestEventAzimuth;
    if (snapshot.layout.directionalRadarAvailable &&
        snapshot.capture.audioFresh &&
        snapshot.capture.state == AudioCaptureState::Running &&
        snapshot.radar.mode != RadarMode::Continuous) {
        const EventDisplaySettings& display = snapshot.settings.eventDisplay;
        const double nowAudio = static_cast<double>(
            snapshot.capture.streamSample) / 48000.0;
        for (const RecentEventSnapshot& event : snapshot.recentEvents) {
            if (!event.liveMarkerEligible || event.streamGeneration != snapshot.capture.streamGeneration ||
                event.peakCount == 0 || event.confidence < display.minimumConfidence ||
                (event.suppressed && !display.showSuppressedEvents) ||
                nowAudio < event.timestampSeconds) {
                continue;
            }
            const float age = static_cast<float>(
                nowAudio - event.timestampSeconds);
            if (age > display.markerPersistenceSeconds) continue;
            const float fade = display.markerPersistenceSeconds > 0.0f
                ? std::clamp(1.0f - age / display.markerPersistenceSeconds,
                             0.0f, 1.0f)
                : 1.0f;
            for (uint32_t index = 0; index < event.peakCount; ++index) {
                const RadarPeak& peak = event.peaks[index];
                const float uncertainty = std::clamp(
                    peak.angularUncertaintyDegrees, 4.0f, 45.0f);
                const float start =
                    (peak.azimuthDegrees - uncertainty - 90.0f) * kPi / 180.0f;
                const float finish =
                    (peak.azimuthDegrees + uncertainty - 90.0f) * kPi / 180.0f;
                const float confidence = std::clamp(peak.confidence, 0.0f, 1.0f);
                const ImU32 color = event.soundClass == SoundClass::Gunshot
                    ? IM_COL32(255, 176, 42,
                               static_cast<int>(255.0f * fade *
                                                std::max(0.18f, confidence)))
                    : IM_COL32(24, 212, 232,
                               static_cast<int>(255.0f * fade *
                                                std::max(0.18f, confidence)));
                drawList->PathArcTo(center, radius * (0.84f - index * 0.06f),
                                    start, finish, 32);
                drawList->PathStroke(color, 0, 4.0f + confidence * 4.0f);
            }
            newestEventAzimuth = event.strongestAzimuthDegrees;
        }
    }

    const bool useContinuousArrow = drawContinuous &&
        snapshot.capture.audioFresh &&
        snapshot.capture.state == AudioCaptureState::Running &&
        snapshot.radar.frame.status == RadarRuntimeStatus::Active;
    const std::optional<float> arrowAzimuth = useContinuousArrow
        ? std::optional<float>(snapshot.radar.frame.strongestAzimuthDegrees)
        : newestEventAzimuth;
    if (arrowAzimuth) {
        const ImVec2 tip = DirectionPoint(center, radius * 0.92f, *arrowAzimuth);
        const ImVec2 left = DirectionPoint(center, radius * 0.70f,
                                           *arrowAzimuth - 5.0f);
        const ImVec2 right = DirectionPoint(center, radius * 0.70f,
                                            *arrowAzimuth + 5.0f);
        drawList->AddTriangleFilled(tip, left, right,
                                    IM_COL32(255, 180, 42, 245));
    }
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(220, 230, 234, 230));
    const char* centerText = !snapshot.layout.directionalRadarAvailable
        ? "DIRECTIONAL RADAR DISABLED"
        : (snapshot.radar.mode == RadarMode::Events && !newestEventAzimuth
            ? "WAITING FOR EVENT"
            : ToString(snapshot.radar.frame.status));
    const ImVec2 textSize = ImGui::CalcTextSize(centerText);
    drawList->AddText({center.x - textSize.x * 0.5f,
                       center.y + radius * 0.52f},
                      snapshot.layout.directionalRadarAvailable
                          ? IM_COL32(130, 155, 164, 240)
                          : IM_COL32(255, 175, 46, 255), centerText);
    ImGui::Dummy(available);
    ImGui::EndChild();
}

void OverlayRenderer::DrawQuickControls(const AppSnapshot& snapshot,
                                        float height) {
    ImGui::BeginChild("QuickControls", ImVec2(0.0f, height), true);
    ImGui::TextUnformatted("QUICK CONTROLS");
    ImGui::Separator();

    const char* modes[]{"Continuous", "Events", "Combined"};
    int mode = static_cast<int>(snapshot.settings.radar.mode);
    ImGui::TextDisabled("Radar mode");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##RadarMode", &mode, modes, IM_ARRAYSIZE(modes)) &&
        m_config.commands) {
        m_config.commands->Emplace<SetRadarModeCommand>(
            static_cast<RadarMode>(mode));
    }
    const char* presets[]{"All", "Footsteps", "Gunshots", "Custom"};
    int preset = static_cast<int>(snapshot.settings.radar.preset);
    ImGui::TextDisabled("Spectral preset");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##RadarPreset", &preset, presets,
                     IM_ARRAYSIZE(presets)) && m_config.commands) {
        m_config.commands->Emplace<SetRadarPresetCommand>(
            static_cast<RadarPreset>(preset));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Frequency emphasis only; this does not classify sounds.");
    }
    float sensitivity = snapshot.settings.radar.sensitivityDbfs;
    ImGui::TextDisabled("Sensitivity");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##RadarSensitivity", &sensitivity,
                           -96.0f, 0.0f, "%.0f dBFS") && m_config.commands) {
        m_config.commands->Emplace<SetRadarSensitivityCommand>(sensitivity);
    }
    float persistence = snapshot.settings.eventDisplay.markerPersistenceSeconds;
    ImGui::TextDisabled("Event persistence");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##EventPersistence", &persistence,
                           0.0f, 10.0f, "%.1f s") && m_config.commands) {
        EventDisplaySettings display = snapshot.settings.eventDisplay;
        display.markerPersistenceSeconds = persistence;
        m_config.commands->Emplace<UpdateEventDisplayCommand>(display);
    }

    bool hudVisible = snapshot.hud.state == HudUiState::Visible ||
                      snapshot.hud.state == HudUiState::Editing;
    if (ImGui::Checkbox("Show in-game HUD", &hudVisible) && m_config.commands) {
        m_config.commands->Emplace<SetHudVisibleCommand>(hudVisible);
    }
    bool recording = snapshot.recording;
    if (ImGui::Checkbox("Record playback WAV", &recording) &&
        m_config.commands) {
        m_config.commands->Emplace<SetRecordingCommand>(recording);
    }
    ImGui::EndChild();
}

void OverlayRenderer::DrawRecentEvents(const AppSnapshot& snapshot) {
    ImGui::TextUnformatted("RECENT EVENTS");
    const EventDisplaySettings& display = snapshot.settings.eventDisplay;
    if (!display.showTimeline) {
        ImGui::TextDisabled(
            "Timeline hidden. Re-enable it under Event Details.");
        return;
    }
    const bool anyVisible = std::any_of(snapshot.recentEvents.begin(), snapshot.recentEvents.end(),
        [&](const RecentEventSnapshot& event) {
            return event.confidence >= display.minimumConfidence &&
                (!event.suppressed || display.showSuppressedEvents);
        });
    if (!anyVisible) {
        ImGui::BeginChild("EmptyTimeline", ImVec2(0, 110.0f * m_appliedUiScale), true);
        ImGui::TextUnformatted(snapshot.model.state == ModelUiState::Ready
            ? "Listening for your next event" : "Event recognition needs a model");
        ImGui::TextWrapped("%s", snapshot.model.state == ModelUiState::Ready
            ? "Recognized footsteps and gunshots appear here. Audio activity is shown independently above."
            : "Start EchoRadar with --model <package-directory> to enable sound classification.");
        ImGui::EndChild();
        return;
    }
    const int columnCount = 4 + (display.showConfidence ? 1 : 0) +
        (display.showMiniRadar ? 1 : 0);
    if (ImGui::BeginTable("RecentEventTimeline", columnCount,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH |
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 180.0f))) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Class");
        if (display.showConfidence) ImGui::TableSetupColumn("Confidence");
        ImGui::TableSetupColumn("Strongest direction");
        ImGui::TableSetupColumn("Peaks");
        if (display.showMiniRadar) ImGui::TableSetupColumn("Mini radar");
        ImGui::TableHeadersRow();
        size_t shown = 0;
        for (auto iterator = snapshot.recentEvents.rbegin();
             iterator != snapshot.recentEvents.rend() && shown < 20;
             ++iterator) {
            const RecentEventSnapshot& event = *iterator;
            if (event.confidence < display.minimumConfidence ||
                (event.suppressed && !display.showSuppressedEvents)) {
                continue;
            }
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.2fs", event.timestampSeconds);
            ImGui::TableNextColumn();
            ImGui::TextColored(event.soundClass == SoundClass::Gunshot
                                   ? kAmber : kCyan,
                               "%s%s", ToString(event.soundClass),
                               event.suppressed ? " [suppressed]" : "");
            if (display.showConfidence) {
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", event.confidence * 100.0f);
            }
            ImGui::TableNextColumn();
            if (event.peakCount == 0) ImGui::TextDisabled("Pending / unavailable");
            else ImGui::Text("%.0f deg", event.strongestAzimuthDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%u", event.peakCount);
            if (display.showMiniRadar) {
                ImGui::TableNextColumn();
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                DrawMiniRadar(ImGui::GetWindowDrawList(),
                              {cell.x + 32.0f, cell.y + 16.0f}, 14.0f,
                              event.miniatureRadarDbfs,
                              snapshot.settings.radar.sensitivityDbfs);
                ImGui::Dummy(ImVec2(64.0f, 32.0f));
            }
        }
        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No recognition-triggered radar events yet.");
        }
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawAudioSetup(const AppSnapshot& snapshot) {
    ImGui::Indent();
    ImGui::TextColored(snapshot.settings.onboarding.completed ? kCyan : kAmber,
                       "%s", snapshot.settings.onboarding.completed ? "Capture setup complete"
                           : SetupStepText(snapshot.settings.onboarding.step));
    const char* preview = snapshot.capture.endpointName.empty()
        ? "Select a Windows playback endpoint" : snapshot.capture.endpointName.c_str();
    ImGui::SetNextItemWidth(std::min(620.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Playback endpoint", preview)) {
        for (const AudioDeviceInfo& endpoint : snapshot.outputDevices) {
            const bool selected = endpoint.id == snapshot.capture.endpointId;
            const std::string label = endpoint.name + "  [" +
                std::to_string(endpoint.nativeChannels) + " ch / " +
                std::to_string(endpoint.nativeSampleRate) + " Hz / " +
                ToString(endpoint.layout.kind) + "]";
            if (ImGui::Selectable(label.c_str(), selected) && m_config.commands) {
                m_config.commands->Emplace<SelectEndpointCommand>(endpoint.id);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        if (snapshot.outputDevices.empty()) {
            ImGui::TextDisabled("No playback endpoints detected.");
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Open Windows sound settings") && m_config.commands) {
        m_config.commands->Emplace<OpenSoundSettingsCommand>();
    }

    ImGui::Text("Detected: %u Hz, %s, mask 0x%08X",
                snapshot.capture.sampleRate,
                ToString(snapshot.layout.detected.kind),
                snapshot.layout.detected.channelMask);
    if (snapshot.layout.directionalRadarAvailable) {
        ImGui::TextColored(kCyan, "[+] Native surround layout validated");
    } else {
        ImGui::TextColored(kAmber, "[!] %s", snapshot.layout.statusText.c_str());
        ImGui::TextWrapped("Headphone stereo is captured as-is. Event recognition and audio activity remain available. Full azimuth requires discrete surround channels; sample-rate conversion alone does not remove channel roles.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("3.5 mm headphone setup");
    ImGui::TextWrapped("Select the sound card output your headphones are plugged into. A KZ ZS10 Pro 2 or Philips SHP9500 does not expose a separate USB playback device. Match the output selected in your game.");
    ImGui::TextWrapped("Use one spatial renderer: the game headphone/HRTF mode, or Windows Sonic / Dolby / DTS. Check the game guidance before combining effects. A headphone mix does not expose the original audio objects to this app.");
    ImGui::Text("Processing: %u Hz / native playback: %u Hz",
                snapshot.capture.processingSampleRate, snapshot.capture.sampleRate);
    ImGui::Spacing();
    ImGui::TextUnformatted("Live labeled channel meters");
    const uint32_t channelCount = std::min<uint32_t>(
        snapshot.capture.levels.channelCount, kMaxAudioChannels);
    if (ImGui::BeginTable("ChannelMeters", std::max(1u, channelCount),
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
        for (uint32_t channel = 0; channel < channelCount; ++channel) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToString(snapshot.layout.detected.roles[channel]));
            const float meter = std::clamp(
                (20.0f * std::log10(std::max(snapshot.capture.levels.peak[channel], 1e-6f)) + 60.0f) / 60.0f, 0.0f, 1.0f);
            char id[32]{};
            std::snprintf(id, sizeof(id), "##channel_%u", channel);
            ImGui::ProgressBar(meter, ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::EndTable();
    }

    if (!snapshot.settings.onboarding.completed && m_config.commands) {
        SetupEvent next = SetupEvent::Retry;
        const char* text = "Continue";
        bool enabled = true;
        switch (snapshot.settings.onboarding.step) {
        case SetupStep::SelectEndpoint:
            next = SetupEvent::EndpointSelected;
            enabled = !snapshot.capture.endpointId.empty();
            break;
        case SetupStep::ValidateFormat:
            next = snapshot.layout.detected.kind == AudioChannelLayoutKind::Stereo
                ? SetupEvent::HeadphoneFormatSupported
                : snapshot.layout.directionalRadarAvailable
                ? SetupEvent::FormatSupported
                : (snapshot.capture.sampleRate != 48000
                    ? SetupEvent::FormatUnsupportedSampleRate
                    : (snapshot.layout.detected.kind == AudioChannelLayoutKind::Stereo
                        ? SetupEvent::FormatUnsupportedStereo
                        : SetupEvent::FormatUnsupportedChannelMask));
            text = (next == SetupEvent::FormatSupported ||
                    next == SetupEvent::HeadphoneFormatSupported)
                ? "Capture format validated" : "Record unsupported format";
            enabled = snapshot.capture.state == AudioCaptureState::Running;
            break;
        case SetupStep::ConfirmChannels:
            next = SetupEvent::ChannelActivityConfirmed;
            text = "I can see channel activity";
            enabled = snapshot.capture.signal.active;
            break;
        case SetupStep::PreviewHud:
            next = SetupEvent::HudPreviewConfirmed;
            text = "Finish setup";
            break;
        case SetupStep::Complete:
            enabled = false;
            break;
        }
        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::Button(text)) {
            m_config.commands->Emplace<ApplySetupEventCommand>(next);
        }
        if (!enabled) ImGui::EndDisabled();
    }
    ImGui::Unindent();
}

void OverlayRenderer::DrawEventDetails(const AppSnapshot& snapshot) {
    ImGui::Indent();
    EventDisplaySettings display = snapshot.settings.eventDisplay;
    bool displayChanged = false;
    displayChanged |= ImGui::Checkbox("Show recent-event timeline",
                                      &display.showTimeline);
    displayChanged |= ImGui::Checkbox("Show confidence column",
                                      &display.showConfidence);
    displayChanged |= ImGui::Checkbox("Show miniature radar",
                                      &display.showMiniRadar);
    ImGui::BeginDisabled();
    ImGui::Checkbox("Show suppressed events", &display.showSuppressedEvents);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("recognizer safety policy excludes them");
    displayChanged |= ImGui::SliderFloat("Minimum event confidence",
                                         &display.minimumConfidence,
                                         0.0f, 1.0f, "%.2f");
    int maximumEvents = static_cast<int>(display.maximumRecentEvents);
    if (ImGui::SliderInt("Maximum retained events", &maximumEvents,
                         1, 500)) {
        display.maximumRecentEvents = static_cast<uint32_t>(maximumEvents);
        displayChanged = true;
    }
    if (displayChanged && m_config.commands) {
        m_config.commands->Emplace<UpdateEventDisplayCommand>(display);
    }
    ImGui::Separator();

    const RecentEventSnapshot* latest = nullptr;
    for (auto iterator = snapshot.recentEvents.rbegin();
         iterator != snapshot.recentEvents.rend(); ++iterator) {
        if (iterator->confidence >= display.minimumConfidence &&
            (!iterator->suppressed || display.showSuppressedEvents)) {
            latest = &*iterator;
            break;
        }
    }
    if (!latest) {
        ImGui::TextDisabled("No event radar result is available.");
        ImGui::Unindent();
        return;
    }
    const RecentEventSnapshot& event = *latest;
    ImGui::Text("Latest %s at %.2fs - recognition confidence %.0f%%",
                ToString(event.soundClass), event.timestampSeconds,
                event.confidence * 100.0f);
    if (ImGui::BeginTable("EventPeaks", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Energy peak");
        ImGui::TableSetupColumn("Azimuth");
        ImGui::TableSetupColumn("Energy");
        ImGui::TableSetupColumn("Angular uncertainty");
        ImGui::TableHeadersRow();
        for (uint32_t index = 0; index < event.peakCount; ++index) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", index + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.0f deg", event.peaks[index].azimuthDegrees);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", event.peaks[index].energyDbfs);
            ImGui::TableNextColumn(); ImGui::Text("+/- %.0f deg", event.peaks[index].angularUncertaintyDegrees);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Peaks are separated azimuthal energy maxima, not inferred physical sources. Elevation is not estimated.");
    ImGui::Unindent();
}

void OverlayRenderer::DrawHudEditor(const AppSnapshot& snapshot) {
    ImGui::Indent();
    const std::string displayId = snapshot.hud.displayId.empty()
        ? "default" : snapshot.hud.displayId;
    HudDisplaySettings hud;
    if (const HudDisplaySettings* found =
            FindHudDisplaySettings(snapshot.settings, displayId)) {
        hud = *found;
    } else if (const HudDisplaySettings* fallback =
                   FindHudDisplaySettings(snapshot.settings, "default")) {
        hud = *fallback;
        hud.displayId = displayId;
    } else {
        hud.displayId = displayId;
    }
    bool changed = false;
    ImGui::Text("Display: %s", displayId.c_str());
    changed |= ImGui::SliderFloat("HUD scale", &hud.scale, 0.25f, 4.0f, "%.2fx");
    changed |= ImGui::SliderFloat("HUD opacity", &hud.opacity, 0.05f, 1.0f, "%.2f");
    const char* anchors[]{"Top left", "Top center", "Top right", "Center left",
                          "Center", "Center right", "Bottom left", "Bottom center",
                          "Bottom right"};
    int anchor = static_cast<int>(hud.anchor);
    if (ImGui::Combo("Anchor", &anchor, anchors, IM_ARRAYSIZE(anchors))) {
        hud.anchor = static_cast<HudAnchor>(anchor);
        changed = true;
    }
    changed |= ImGui::DragFloat2("Offset", &hud.offsetX, 1.0f, -4000.0f,
                                 4000.0f, "%.0f px");
    changed |= ImGui::ColorEdit4("Sector color", &hud.sectorColor.red,
                                 ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::ColorEdit4("Strongest arrow", &hud.strongestColor.red,
                                 ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::Checkbox("Cardinal labels", &hud.showCardinalLabels);
    changed |= ImGui::Checkbox("Degree labels", &hud.showDegreeLabels);
    changed |= ImGui::SliderFloat("Marker persistence", &hud.persistenceSeconds,
                                  0.0f, 10.0f, "%.1f s");
    const char* backgrounds[]{"Transparent", "Tactical dark", "Light", "Checkerboard"};
    int background = static_cast<int>(hud.previewBackground);
    if (ImGui::Combo("Preview background", &background, backgrounds,
                     IM_ARRAYSIZE(backgrounds))) {
        hud.previewBackground = static_cast<HudPreviewBackground>(background);
        changed = true;
    }
    if (changed && m_config.commands) {
        m_config.commands->Emplace<UpdateHudDisplayCommand>(displayId, hud);
    }
    bool edit = snapshot.hud.state == HudUiState::Editing;
    if (ImGui::Checkbox("Unlock HUD for drag positioning", &edit) &&
        m_config.commands) {
        m_config.commands->Emplace<SetHudEditModeCommand>(displayId, edit);
    }
    ImGui::TextDisabled(edit
        ? "Edit mode is active: the topmost HUD accepts dragging. Turn it off to restore click-through."
        : "Normal mode is topmost and click-through.");
    ImGui::Unindent();
}

void OverlayRenderer::DrawAdvanced(const AppSnapshot& snapshot) {
    ImGui::Indent();
    RadarProcessorConfig tuning = snapshot.settings.radar;
    bool smoothingChanged = false;
    int attack = static_cast<int>(tuning.attackMilliseconds);
    int release = static_cast<int>(tuning.releaseMilliseconds);
    int hold = static_cast<int>(tuning.holdMilliseconds);
    smoothingChanged |= ImGui::SliderInt("Attack", &attack, 0, 2000, "%d ms");
    smoothingChanged |= ImGui::SliderInt("Release", &release, 0, 5000, "%d ms");
    smoothingChanged |= ImGui::SliderInt("Hold", &hold, 0, 2000, "%d ms");
    if (smoothingChanged && m_config.commands) {
        m_config.commands->Emplace<SetRadarSmoothingCommand>(
            static_cast<uint32_t>(attack), static_cast<uint32_t>(release),
            static_cast<uint32_t>(hold));
    }
    ImGui::TextDisabled("STFT: 2048 Hann / 480 sample (10 ms) hop / 24 sectors");

    if (tuning.preset == RadarPreset::Custom) {
        ImGui::TextUnformatted("Custom spectral emphasis (dB)");
        bool curveChanged = false;
        for (size_t index = 0; index < tuning.customCurveDb.size(); ++index) {
            char label[48]{};
            std::snprintf(label, sizeof(label), "%.0f Hz##curve_%zu",
                          kRadarCurveFrequenciesHz[index], index);
            curveChanged |= ImGui::SliderFloat(
                label, &tuning.customCurveDb[index], -24.0f, 24.0f, "%+.1f dB");
        }
        if (curveChanged && m_config.commands) {
            m_config.commands->Emplace<SetCustomRadarCurveCommand>(
                tuning.customCurveDb);
        }
    }
    ImGui::Separator();
    ImGui::TextWrapped("Recognition: %s", snapshot.model.statusText.c_str());
    ImGui::Text("Gunshot %.2f / Footstep %.2f / Self-suppressed %llu",
        snapshot.model.probabilities[0], snapshot.model.probabilities[1],
        static_cast<unsigned long long>(snapshot.model.suppressedEvents));
    ImGui::TextWrapped("Recognition runs in every radar mode when a model is loaded. Suppression and class thresholds are independent of the audio activity meter.");
    if (snapshot.model.state == ModelUiState::Ready) {
        RecognitionRuntimeTuning recognition = snapshot.recognitionTuning;
        bool recognitionChanged = false;
        ImGui::TextUnformatted("Recognition thresholds");
        recognitionChanged |= ImGui::SliderFloat(
            "Gunshot threshold (quiet)", &recognition.quietThresholds[0],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Footstep threshold (quiet)", &recognition.quietThresholds[1],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Gunshot threshold (busy)", &recognition.busyThresholds[0],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Footstep threshold (busy)", &recognition.busyThresholds[1],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Scene activity cutoff", &recognition.sceneActivityCutoff,
            0.01f, 0.99f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Self-suppression threshold",
            &recognition.selfSuppressionThreshold, 0.01f, 1.0f, "%.2f");
        int gunshotSpacing = static_cast<int>(
            recognition.minimumSpacingMs[0]);
        int footstepSpacing = static_cast<int>(
            recognition.minimumSpacingMs[1]);
        if (ImGui::SliderInt("Gunshot minimum spacing", &gunshotSpacing,
                             1, 5000, "%d ms")) {
            recognition.minimumSpacingMs[0] =
                static_cast<uint32_t>(gunshotSpacing);
            recognitionChanged = true;
        }
        if (ImGui::SliderInt("Footstep minimum spacing", &footstepSpacing,
                             1, 5000, "%d ms")) {
            recognition.minimumSpacingMs[1] =
                static_cast<uint32_t>(footstepSpacing);
            recognitionChanged = true;
        }
        if (recognitionChanged && m_config.commands) {
            m_config.commands->Emplace<UpdateRecognitionTuningCommand>(
                recognition);
        }
    } else {
        ImGui::TextDisabled(
            "Load a recognition package with --model to edit thresholds.");
    }
    bool sessionLogging = snapshot.sessionLoggingActive;
    if (ImGui::Checkbox("Write schema-3 session log", &sessionLogging) &&
        m_config.commands) {
        m_config.commands->Emplace<SetSessionLoggingCommand>(sessionLogging);
    }
    if (!snapshot.sessionLoggingError.empty()) {
        ImGui::TextColored(kRed, "! %s",
                           snapshot.sessionLoggingError.c_str());
    }
    ImGui::Text("Stream generation: %llu",
                static_cast<unsigned long long>(snapshot.capture.streamGeneration));
    ImGui::Text("Capture drops: %llu frames",
                static_cast<unsigned long long>(snapshot.capture.droppedFrames));
    ImGui::Text("Backlog discarded: %llu frames",
                static_cast<unsigned long long>(
                    snapshot.capture.discardedBacklogFrames));
    ImGui::Text("Device restarts: %llu",
                static_cast<unsigned long long>(snapshot.capture.restartCount));
    ImGui::Text("Channel mask: 0x%08X", snapshot.layout.detected.channelMask);
    ImGui::Text("Radar confidence: %.3f", snapshot.radar.frame.confidence);
    if (m_config.commands && ImGui::Button("Save settings now")) {
        m_config.commands->Emplace<SaveSettingsCommand>();
    }
    ImGui::Unindent();
}

} // namespace EchoRadar
