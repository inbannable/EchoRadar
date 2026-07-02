#include "DatasetStudioPanel.h"

#include "dataset/DatasetJson.h"
#include "dataset/DatasetWavStats.h"
#include "dsp/STFTProcessor.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>

#include <imgui.h>

using namespace EchoRadar;

namespace {

constexpr std::array<DatasetLabel, 6> kLabels = {
    DatasetLabel::Gunshot,
    DatasetLabel::Footstep,
    DatasetLabel::Reload,
    DatasetLabel::Switch,
    DatasetLabel::Ambient,
    DatasetLabel::Unknown,
};

constexpr const char* kLabelDisplayNames[] = {
    "Gunshot",
    "Footstep",
    "Reload",
    "Switch",
    "Ambient",
    "Unknown",
};

constexpr const char* kLabelFilterItems[] = {
    "All",
    "Gunshot",
    "Footstep",
    "Reload",
    "Switch",
    "Ambient",
    "Unknown",
};

std::string EscapeMciString(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 4);
    for (char c : path) {
        if (c == '\"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool MciSend(const std::string& command) {
    return mciSendStringA(command.c_str(), nullptr, 0, nullptr) == 0;
}

struct MciPlayerState {
    std::string alias{"echoradar_dataset_studio"};
    std::string currentPath;
    bool open{false};
    bool paused{false};
};

MciPlayerState& PlayerState() {
    static MciPlayerState state;
    return state;
}

bool OpenAudioPath(const std::string& path) {
    MciPlayerState& state = PlayerState();
    if (state.open) {
        MciSend("close " + state.alias);
        state.open = false;
        state.paused = false;
    }
    const std::string cmd = "open \"" + EscapeMciString(path) + "\" type waveaudio alias " + state.alias;
    if (!MciSend(cmd)) {
        return false;
    }
    MciSend("set " + state.alias + " time format milliseconds");
    state.currentPath = path;
    state.open = true;
    state.paused = false;
    return true;
}

bool EnsureOpen(const std::string& path) {
    MciPlayerState& state = PlayerState();
    if (state.open && state.currentPath == path) {
        return true;
    }
    return OpenAudioPath(path);
}

bool PlayAudioPath(const std::string& path) {
    if (!EnsureOpen(path)) {
        return false;
    }
    MciPlayerState& state = PlayerState();
    state.paused = false;
    return MciSend("play " + state.alias);
}

bool ReplayAudioPath(const std::string& path) {
    if (!EnsureOpen(path)) {
        return false;
    }
    MciPlayerState& state = PlayerState();
    state.paused = false;
    return MciSend("seek " + state.alias + " to start") && MciSend("play " + state.alias);
}

bool PauseAudio() {
    MciPlayerState& state = PlayerState();
    if (!state.open) {
        return false;
    }
    state.paused = true;
    return MciSend("pause " + state.alias);
}

bool StopAudioPlayback() {
    MciPlayerState& state = PlayerState();
    if (!state.open) {
        return false;
    }
    state.paused = false;
    return MciSend("stop " + state.alias);
}

bool SeekAudioRelative(int deltaMs) {
    MciPlayerState& state = PlayerState();
    if (!state.open) {
        return false;
    }
    char buffer[128] = {};
    if (mciSendStringA(("status " + state.alias + " position").c_str(), buffer, sizeof(buffer), nullptr) != 0) {
        return false;
    }
    const int current = std::atoi(buffer);
    const int target = std::max(0, current + deltaMs);
    std::ostringstream seekCmd;
    seekCmd << "seek " << state.alias << " to " << target;
    if (!MciSend(seekCmd.str())) {
        return false;
    }
    if (!state.paused) {
        return MciSend("play " + state.alias);
    }
    return true;
}

std::string LowerAscii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    const std::string h = LowerAscii(haystack);
    const std::string n = LowerAscii(needle);
    return h.find(n) != std::string::npos;
}

std::vector<std::string> ReadCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (c == ',') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

void LoadFeatureCsv(const std::string& path,
                    std::vector<std::string>& names,
                    std::vector<std::vector<float>>& columns) {
    names.clear();
    columns.clear();

    std::ifstream in(path);
    if (!in) {
        return;
    }

    std::string line;
    if (!std::getline(in, line)) {
        return;
    }
    names = ReadCsvLine(line);
    columns.resize(names.size());

    while (std::getline(in, line)) {
        const auto cells = ReadCsvLine(line);
        for (size_t i = 0; i < cells.size() && i < columns.size(); ++i) {
            try {
                columns[i].push_back(std::stof(cells[i]));
            } catch (...) {
                columns[i].push_back(0.0f);
            }
        }
        for (size_t i = cells.size(); i < columns.size(); ++i) {
            columns[i].push_back(0.0f);
        }
    }
}

void BuildSpectrogram(const DatasetEventRecord& record,
                      const std::vector<float>& interleaved,
                      int& outFrames,
                      int& outBins,
                      float& outMin,
                      float& outMax,
                      std::vector<float>& outValues) {
    outFrames = 0;
    outBins = 0;
    outMin = -6.0f;
    outMax = 0.0f;
    outValues.clear();

    if (interleaved.empty()) {
        return;
    }

    STFTProcessor stft({record.fftSize, record.hopSize, record.sampleRate});
    stft.PushInterleaved(interleaved.data(), interleaved.size() / 2);
    STFTFrame frame{};
    float vMin = 1e9f;
    float vMax = -1e9f;

    while (stft.PopFrame(frame)) {
        if (outBins == 0) {
            outBins = static_cast<int>(frame.left.magnitudes.size());
        }
        if (outBins <= 0) {
            continue;
        }
        ++outFrames;
        for (int b = 0; b < outBins; ++b) {
            const float value = std::log10(1e-6f + frame.left.magnitudes[static_cast<size_t>(b)]);
            outValues.push_back(value);
            vMin = std::min(vMin, value);
            vMax = std::max(vMax, value);
        }
    }

    if (vMin < vMax) {
        outMin = vMin;
        outMax = vMax;
    }
}

ImU32 HeatColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const int r = static_cast<int>(255.0f * t);
    const int g = static_cast<int>(255.0f * std::sqrt(t));
    const int b = static_cast<int>(255.0f * (1.0f - t));
    return IM_COL32(r, g, b, 255);
}

const char* LabelDisplayName(DatasetLabel label) {
    switch (label) {
    case DatasetLabel::Gunshot: return "Gunshot";
    case DatasetLabel::Footstep: return "Footstep";
    case DatasetLabel::Reload: return "Reload";
    case DatasetLabel::Switch: return "Switch";
    case DatasetLabel::Ambient: return "Ambient";
    case DatasetLabel::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string FormatTimestamp(uint64_t timestampMs) {
    std::ostringstream out;
    out << timestampMs << " ms";
    return out.str();
}

} // namespace

DatasetStudioPanel::DatasetStudioPanel(std::string datasetRoot)
    : m_manager(std::move(datasetRoot)) {
    std::memset(m_notesBuffer, 0, sizeof(m_notesBuffer));
    RefreshIndex();
}

void DatasetStudioPanel::ToastStatus(const std::string& message, bool ok) {
    m_statusMessage = message;
    m_statusOk = ok;
}

void DatasetStudioPanel::RefreshIndex() {
    const DatasetOpResult result = m_manager.Scan();
    if (!result.ok) {
        ToastStatus(result.message, false);
    }

    RecomputeFilteredList();

    bool selectedStillExists = false;
    for (const auto& ev : m_filtered) {
        if (ev.id == m_selectedId) {
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        m_selectedId.clear();
        m_multiSelect.clear();
        if (!m_filtered.empty()) {
            SelectEvent(m_filtered.front().id);
        } else {
            m_previewDirty = true;
        }
    }
}

void DatasetStudioPanel::RecomputeFilteredList() {
    m_filtered.clear();
    const auto& all = m_manager.GetEvents();
    for (const auto& ev : all) {
        if (m_labelFilterIndex > 0) {
            const DatasetLabel wanted = kLabels[static_cast<size_t>(m_labelFilterIndex - 1)];
            if (ev.label != wanted) {
                continue;
            }
        }
        if (ev.candidateScore < m_minScoreFilter) {
            continue;
        }
        if (ev.confidence < m_minConfidenceFilter) {
            continue;
        }
        if (!ContainsCaseInsensitive(ev.id, m_searchText) &&
            !ContainsCaseInsensitive(ev.eventType, m_searchText) &&
            !ContainsCaseInsensitive(ev.deviceName, m_searchText) &&
            !ContainsCaseInsensitive(ev.notes, m_searchText)) {
            continue;
        }
        m_filtered.push_back(ev);
    }
}

void DatasetStudioPanel::SelectEvent(const std::string& id) {
    m_selectedId = id;
    if (m_multiSelect.empty()) {
        m_multiSelect.insert(id);
    }
    m_previewDirty = true;
}

void DatasetStudioPanel::LoadPreviewForSelected() {
    m_previewDirty = false;
    m_waveformLeft.clear();
    m_waveformRight.clear();
    m_spectrogramLog.clear();
    m_featureColumnNames.clear();
    m_featureColumns.clear();
    std::memset(m_notesBuffer, 0, sizeof(m_notesBuffer));
    m_audioDurationSec = 0.0;

    if (m_selectedId.empty()) {
        return;
    }

    const auto selected = m_manager.GetEvent(m_selectedId);
    if (!selected.has_value()) {
        return;
    }

    const detail::WavSamples wav = detail::ReadWavFullPcm16(selected->audioPath);
    if (wav.ok && wav.channels >= 2) {
        m_waveformLeft.reserve(wav.interleaved.size() / wav.channels);
        m_waveformRight.reserve(wav.interleaved.size() / wav.channels);
        for (size_t i = 0; i + 1 < wav.interleaved.size(); i += wav.channels) {
            m_waveformLeft.push_back(wav.interleaved[i]);
            m_waveformRight.push_back(wav.interleaved[i + 1]);
        }
        if (wav.sampleRate > 0) {
            m_audioDurationSec = static_cast<double>(m_waveformLeft.size()) / static_cast<double>(wav.sampleRate);
        }
        BuildSpectrogram(*selected,
                         wav.interleaved,
                         m_specFrames,
                         m_specBins,
                         m_specMin,
                         m_specMax,
                         m_spectrogramLog);
    } else {
        m_specFrames = 0;
        m_specBins = 0;
        m_specMin = -6.0f;
        m_specMax = 0.0f;
    }

    LoadFeatureCsv(selected->csvPath, m_featureColumnNames, m_featureColumns);
    std::snprintf(m_notesBuffer, sizeof(m_notesBuffer), "%s", selected->notes.c_str());
    m_waveformZoom = 1.0f;
    m_waveformOffset = 0.0f;
    m_spectrogramZoom = 1.0f;
    m_spectrogramOffset = 0.0f;
}

void DatasetStudioPanel::PushReplay(const std::string& id) {
    if (id.empty()) {
        return;
    }
    m_replayQueue.erase(std::remove(m_replayQueue.begin(), m_replayQueue.end(), id), m_replayQueue.end());
    m_replayQueue.push_front(id);
    while (m_replayQueue.size() > kReplayQueueMax) {
        m_replayQueue.pop_back();
    }
}

void DatasetStudioPanel::PlaySelectedAudio() {
    const auto selected = m_manager.GetEvent(m_selectedId);
    if (!selected.has_value()) {
        ToastStatus("No event selected", false);
        return;
    }
    if (PlayAudioPath(selected->audioPath)) {
        m_isPlaying = true;
        PushReplay(selected->id);
        ToastStatus("Playing " + selected->id, true);
    } else {
        ToastStatus("Failed to play audio for " + selected->id, false);
    }
}

void DatasetStudioPanel::StopAudio() {
    StopAudioPlayback();
    m_isPlaying = false;
}

void DatasetStudioPanel::ApplyLabelToSelection(DatasetLabel label) {
    std::vector<std::string> ids(m_multiSelect.begin(), m_multiSelect.end());
    if (ids.empty() && !m_selectedId.empty()) {
        ids.push_back(m_selectedId);
    }
    if (ids.empty()) {
        ToastStatus("No event selected", false);
        return;
    }

    int moved = 0;
    for (const std::string& id : ids) {
        const auto before = m_manager.GetEvent(id);
        if (!before.has_value()) {
            continue;
        }
        const DatasetOpResult result = m_manager.MoveLabel(id, label);
        if (result.ok && before->label != label) {
            ++moved;
        }
        if (!result.ok) {
            ToastStatus(result.message, false);
            RefreshIndex();
            return;
        }
    }

    m_sessionMoved += moved;
    m_sessionLabeled += moved;
    RefreshIndex();
    if (!ids.empty()) {
        SelectEvent(ids.front());
    }
    ToastStatus("Moved " + std::to_string(moved) + " event(s) to " + LabelDisplayName(label), true);
}

void DatasetStudioPanel::DeleteSelection() {
    std::vector<std::string> ids(m_multiSelect.begin(), m_multiSelect.end());
    if (ids.empty() && !m_selectedId.empty()) {
        ids.push_back(m_selectedId);
    }
    if (ids.empty()) {
        ToastStatus("No event selected", false);
        return;
    }

    int deleted = 0;
    for (const std::string& id : ids) {
        const DatasetOpResult result = m_manager.Delete(id);
        if (!result.ok) {
            ToastStatus(result.message, false);
            RefreshIndex();
            return;
        }
        ++deleted;
    }

    m_sessionDeleted += deleted;
    m_selectedId.clear();
    m_multiSelect.clear();
    RefreshIndex();
    ToastStatus("Deleted " + std::to_string(deleted) + " event(s)", true);
}

void DatasetStudioPanel::RunUndo() {
    const DatasetOpResult result = m_manager.Undo();
    if (!result.ok) {
        ToastStatus(result.message, false);
        return;
    }
    ++m_sessionUndone;
    RefreshIndex();
    ToastStatus(result.message, true);
}

void DatasetStudioPanel::HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (m_isPlaying) {
            if (PauseAudio()) {
                m_isPlaying = false;
                ToastStatus("Paused", true);
            }
        } else {
            PlaySelectedAudio();
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        RunUndo();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        m_confirmDeletePending = true;
        ImGui::OpenPopup("Delete Event?");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) ApplyLabelToSelection(DatasetLabel::Gunshot);
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) ApplyLabelToSelection(DatasetLabel::Footstep);
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) ApplyLabelToSelection(DatasetLabel::Reload);
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) ApplyLabelToSelection(DatasetLabel::Switch);
    if (ImGui::IsKeyPressed(ImGuiKey_5, false)) ApplyLabelToSelection(DatasetLabel::Ambient);
    if (ImGui::IsKeyPressed(ImGuiKey_6, false)) ApplyLabelToSelection(DatasetLabel::Unknown);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
        SeekAudioRelative(-100);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        SeekAudioRelative(100);
    }
}

void DatasetStudioPanel::RenderToolbar() {
    if (ImGui::Button("Refresh")) {
        RefreshIndex();
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo")) {
        RunUndo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Play")) {
        PlaySelectedAudio();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
        if (PauseAudio()) {
            m_isPlaying = false;
            ToastStatus("Paused", true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        StopAudio();
        ToastStatus("Stopped", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Replay")) {
        const auto selected = m_manager.GetEvent(m_selectedId);
        if (selected.has_value()) {
            if (ReplayAudioPath(selected->audioPath)) {
                m_isPlaying = true;
                PushReplay(selected->id);
                ToastStatus("Replay " + selected->id, true);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        m_confirmDeletePending = true;
        ImGui::OpenPopup("Delete Event?");
    }

    for (size_t i = 0; i < std::size(kLabels); ++i) {
        ImGui::SameLine();
        if (ImGui::Button(kLabelDisplayNames[i])) {
            ApplyLabelToSelection(kLabels[i]);
        }
    }

    char searchBuffer[256] = {};
    std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", m_searchText.c_str());
    if (ImGui::InputText("Search", searchBuffer, sizeof(searchBuffer))) {
        m_searchText = searchBuffer;
        RecomputeFilteredList();
    }
    ImGui::SameLine();
    if (ImGui::Combo("Label Filter", &m_labelFilterIndex, kLabelFilterItems, static_cast<int>(std::size(kLabelFilterItems)))) {
        RecomputeFilteredList();
    }
    if (ImGui::SliderFloat("Min Score", &m_minScoreFilter, 0.0f, 1.0f, "%.2f")) {
        RecomputeFilteredList();
    }
    if (ImGui::SliderFloat("Min Confidence", &m_minConfidenceFilter, 0.0f, 1.0f, "%.2f")) {
        RecomputeFilteredList();
    }

    if (!m_statusMessage.empty()) {
        ImGui::TextColored(m_statusOk ? ImVec4(0.45f, 0.9f, 0.55f, 1.0f)
                                      : ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "%s",
                           m_statusMessage.c_str());
    }
}

void DatasetStudioPanel::RenderBrowser() {
    ImGui::BeginChild("dataset_browser", ImVec2(280.0f, 0.0f), true);
    ImGui::TextUnformatted("Dataset");
    ImGui::Separator();

    std::map<DatasetLabel, std::vector<DatasetEventRecord>> grouped;
    for (const auto& ev : m_filtered) {
        grouped[ev.label].push_back(ev);
    }

    for (DatasetLabel label : kLabels) {
        std::ostringstream title;
        const auto it = grouped.find(label);
        const size_t count = (it == grouped.end()) ? 0u : it->second.size();
        title << LabelDisplayName(label) << " (" << count << ")";
        if (ImGui::CollapsingHeader(title.str().c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (it != grouped.end()) {
                for (const auto& ev : it->second) {
                    const bool selected = m_multiSelect.count(ev.id) > 0;
                    if (ImGui::Selectable(ev.id.c_str(), selected)) {
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        const bool shift = ImGui::GetIO().KeyShift;
                        if (shift && !m_lastAnchorId.empty()) {
                            m_multiSelect.clear();
                            int start = -1;
                            int end = -1;
                            for (size_t i = 0; i < m_filtered.size(); ++i) {
                                if (m_filtered[i].id == m_lastAnchorId) start = static_cast<int>(i);
                                if (m_filtered[i].id == ev.id) end = static_cast<int>(i);
                            }
                            if (start >= 0 && end >= 0) {
                                if (start > end) std::swap(start, end);
                                for (int i = start; i <= end; ++i) {
                                    m_multiSelect.insert(m_filtered[static_cast<size_t>(i)].id);
                                }
                            } else {
                                m_multiSelect.insert(ev.id);
                            }
                        } else if (ctrl) {
                            if (selected) {
                                m_multiSelect.erase(ev.id);
                            } else {
                                m_multiSelect.insert(ev.id);
                            }
                            m_lastAnchorId = ev.id;
                        } else {
                            m_multiSelect.clear();
                            m_multiSelect.insert(ev.id);
                            m_lastAnchorId = ev.id;
                        }
                        SelectEvent(ev.id);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("score %.2f | conf %.2f | %s",
                                          ev.candidateScore,
                                          ev.confidence,
                                          ev.eventType.c_str());
                    }
                }
            }
        }
    }
    ImGui::EndChild();
}

void DatasetStudioPanel::RenderWaveformSection() {
    ImGui::SeparatorText("Waveform");
    if (m_waveformLeft.empty()) {
        ImGui::TextDisabled("No waveform loaded");
        return;
    }

    const ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 180.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("waveform_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(22, 24, 30, 255), 2.0f);
    draw->AddRect(p0, p1, IM_COL32(80, 85, 95, 255), 2.0f);

    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            m_waveformZoom = std::clamp(m_waveformZoom * (wheel > 0.0f ? 1.15f : 0.87f), 1.0f, 32.0f);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        const float span = 1.0f / m_waveformZoom;
        m_waveformOffset = std::clamp(m_waveformOffset - ImGui::GetIO().MouseDelta.x / std::max(1.0f, canvasSize.x) * span, 0.0f, std::max(0.0f, 1.0f - span));
    }

    const size_t total = std::min(m_waveformLeft.size(), m_waveformRight.size());
    const float visibleFrac = 1.0f / m_waveformZoom;
    const size_t start = static_cast<size_t>(m_waveformOffset * static_cast<float>(total));
    const size_t visible = std::max<size_t>(64, static_cast<size_t>(visibleFrac * static_cast<float>(total)));
    const size_t end = std::min(total, start + visible);
    const float midY = (p0.y + p1.y) * 0.5f;
    const float amp = (p1.y - p0.y) * 0.42f;

    auto drawChannel = [&](const std::vector<float>& samples, ImU32 color) {
        if (end <= start + 1) {
            return;
        }
        const float dx = (p1.x - p0.x) / static_cast<float>(end - start - 1);
        for (size_t i = start + 1; i < end; ++i) {
            const float x0 = p0.x + dx * static_cast<float>((i - 1) - start);
            const float x1 = p0.x + dx * static_cast<float>(i - start);
            const float y0 = midY - samples[i - 1] * amp;
            const float y1 = midY - samples[i] * amp;
            draw->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 1.0f);
        }
    };

    draw->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), IM_COL32(80, 80, 90, 255), 1.0f);
    drawChannel(m_waveformLeft, IM_COL32(90, 220, 255, 255));
    drawChannel(m_waveformRight, IM_COL32(255, 180, 90, 180));

    if (ImGui::IsItemHovered()) {
        const float mouseFrac = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / std::max(1.0f, p1.x - p0.x), 0.0f, 1.0f);
        const size_t sampleIdx = std::min(end - 1, start + static_cast<size_t>(mouseFrac * static_cast<float>(std::max<size_t>(1, end - start))));
        const double timeSec = (total > 0) ? (static_cast<double>(sampleIdx) / static_cast<double>(total)) * m_audioDurationSec : 0.0;
        ImGui::SetTooltip("t=%.3fs | L=%.3f | R=%.3f", timeSec, m_waveformLeft[sampleIdx], m_waveformRight[sampleIdx]);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        MciPlayerState& state = PlayerState();
        if (state.open) {
            const float mouseFrac = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / std::max(1.0f, p1.x - p0.x), 0.0f, 1.0f);
            const double absoluteFrac = (static_cast<double>(start) + static_cast<double>(end - start) * mouseFrac) / static_cast<double>(std::max<size_t>(1, total));
            const int ms = static_cast<int>(absoluteFrac * m_audioDurationSec * 1000.0);
            std::ostringstream cmd;
            cmd << "seek " << state.alias << " to " << ms;
            MciSend(cmd.str());
            if (m_isPlaying) {
                MciSend("play " + state.alias);
            }
        }
    }

    ImGui::TextDisabled("Mouse wheel = zoom | Right drag = pan | Left click = seek | Duration %.0f ms",
                        m_audioDurationSec * 1000.0);
}

void DatasetStudioPanel::RenderSpectrogramSection() {
    ImGui::SeparatorText("Spectrogram");
    if (m_specFrames <= 0 || m_specBins <= 0 || m_spectrogramLog.empty()) {
        ImGui::TextDisabled("No spectrogram loaded");
        return;
    }

    const ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 220.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("spectrogram_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 2.0f);
    draw->AddRect(p0, p1, IM_COL32(80, 80, 90, 255), 2.0f);

    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            m_spectrogramZoom = std::clamp(m_spectrogramZoom * (wheel > 0.0f ? 1.15f : 0.87f), 1.0f, 32.0f);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        const float span = 1.0f / m_spectrogramZoom;
        m_spectrogramOffset = std::clamp(m_spectrogramOffset - ImGui::GetIO().MouseDelta.x / std::max(1.0f, canvasSize.x) * span, 0.0f, std::max(0.0f, 1.0f - span));
    }

    const int visibleFrames = std::max(4, static_cast<int>(static_cast<float>(m_specFrames) / m_spectrogramZoom));
    const int startFrame = std::clamp(static_cast<int>(m_spectrogramOffset * static_cast<float>(m_specFrames)), 0, std::max(0, m_specFrames - visibleFrames));
    const int endFrame = std::min(m_specFrames, startFrame + visibleFrames);
    const int displayBins = std::min(m_specBins, 160);
    const float dx = (p1.x - p0.x) / static_cast<float>(std::max(1, endFrame - startFrame));
    const float dy = (p1.y - p0.y) / static_cast<float>(std::max(1, displayBins));
    const float denom = std::max(1e-6f, m_specMax - m_specMin);

    for (int x = startFrame; x < endFrame; ++x) {
        for (int b = 0; b < displayBins; ++b) {
            const int srcBin = static_cast<int>((static_cast<double>(b) / displayBins) * m_specBins);
            const size_t idx = static_cast<size_t>(x * m_specBins + std::clamp(srcBin, 0, m_specBins - 1));
            const float value = m_spectrogramLog[idx];
            const float t = (value - m_specMin) / denom;
            const float x0 = p0.x + dx * static_cast<float>(x - startFrame);
            const float y0 = p1.y - dy * static_cast<float>(b + 1);
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + dx + 1.0f, y0 + dy + 1.0f), HeatColor(t));
        }
    }

    if (ImGui::IsItemHovered()) {
        const float fx = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / std::max(1.0f, p1.x - p0.x), 0.0f, 1.0f);
        const float fy = std::clamp((ImGui::GetIO().MousePos.y - p0.y) / std::max(1.0f, p1.y - p0.y), 0.0f, 1.0f);
        const int frame = std::clamp(startFrame + static_cast<int>(fx * static_cast<float>(std::max(1, endFrame - startFrame))), 0, m_specFrames - 1);
        const int bin = std::clamp(static_cast<int>((1.0f - fy) * static_cast<float>(displayBins)), 0, displayBins - 1);
        const int srcBin = static_cast<int>((static_cast<double>(bin) / displayBins) * m_specBins);
        const size_t idx = static_cast<size_t>(frame * m_specBins + std::clamp(srcBin, 0, m_specBins - 1));
        const double timeSec = m_audioDurationSec > 0.0 ? (static_cast<double>(frame) / static_cast<double>(m_specFrames)) * m_audioDurationSec : 0.0;
        const double freqHz = (m_specBins > 1) ? (static_cast<double>(srcBin) / static_cast<double>(m_specBins - 1)) * 24000.0 : 0.0;
        ImGui::SetTooltip("t=%.3fs | f=%.0f Hz | mag=%.3f", timeSec, freqHz, m_spectrogramLog[idx]);
    }

    ImGui::TextDisabled("Mouse wheel = zoom | Right drag = pan");
}

void DatasetStudioPanel::RenderFeatureSection() {
    ImGui::SeparatorText("Feature Curves");
    if (m_featureColumnNames.empty() || m_featureColumns.empty()) {
        ImGui::TextDisabled("No feature CSV loaded");
        return;
    }

    for (size_t i = 1; i < m_featureColumnNames.size() && i < m_featureColumns.size(); ++i) {
        if (m_featureColumns[i].empty()) {
            continue;
        }
        ImGui::PlotLines(m_featureColumnNames[i].c_str(),
                         m_featureColumns[i].data(),
                         static_cast<int>(m_featureColumns[i].size()),
                         0,
                         nullptr,
                         FLT_MAX,
                         FLT_MAX,
                         ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));
    }
}

void DatasetStudioPanel::RenderMetadataSection() {
    const auto selected = m_manager.GetEvent(m_selectedId);
    ImGui::SeparatorText("Metadata");
    if (!selected.has_value()) {
        ImGui::TextDisabled("No event selected");
        return;
    }

    ImGui::Text("ID: %s", selected->id.c_str());
    ImGui::Text("Label: %s", LabelDisplayName(selected->label));
    ImGui::Text("Type: %s", selected->eventType.c_str());
    ImGui::Text("Timestamp: %s", FormatTimestamp(selected->timestampMs).c_str());
    ImGui::Text("Score: %.3f", selected->candidateScore);
    ImGui::Text("Confidence: %.3f", selected->confidence);
    ImGui::Text("Threshold: %.3f", selected->triggerThreshold);
    ImGui::Text("Device: %s", selected->deviceName.c_str());
    ImGui::Text("FFT/Hop: %u / %u", selected->fftSize, selected->hopSize);
    ImGui::Text("Audio: %s", selected->audioPath.c_str());

    if (ImGui::InputTextMultiline("Notes", m_notesBuffer, sizeof(m_notesBuffer), ImVec2(-1.0f, 80.0f))) {
    }
    if (ImGui::Button("Save Notes")) {
        const DatasetOpResult result = m_manager.UpdateNotes(selected->id, m_notesBuffer);
        ToastStatus(result.message, result.ok);
        if (result.ok) {
            RefreshIndex();
        }
    }
}

void DatasetStudioPanel::RenderStatisticsSection() {
    ImGui::SeparatorText("Statistics");
    const auto stats = m_manager.GetStatistics();
    std::array<float, 6> values{};
    for (size_t i = 0; i < std::size(kLabels); ++i) {
        values[i] = static_cast<float>(stats.at(ToString(kLabels[i])));
        ImGui::Text("%s: %.0f", kLabelDisplayNames[i], values[i]);
    }
    ImGui::PlotHistogram("##dataset_counts",
                         values.data(),
                         static_cast<int>(values.size()),
                         0,
                         "Category counts",
                         0.0f,
                         *std::max_element(values.begin(), values.end()) + 1.0f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 80.0f));
}

void DatasetStudioPanel::RenderQualitySection() {
    ImGui::SeparatorText("Quality Check");
    if (ImGui::Button("Find Duplicate")) {
        m_qualityResults = m_manager.FindDuplicates();
        m_qualityResultsTitle = "Duplicate audio";
    }
    ImGui::SameLine();
    if (ImGui::Button("Find Empty")) {
        m_qualityResults = m_manager.FindEmpty();
        m_qualityResultsTitle = "Empty or unreadable clips";
    }
    ImGui::SameLine();
    if (ImGui::Button("Find Very Short")) {
        m_qualityResults = m_manager.FindVeryShort();
        m_qualityResultsTitle = "Very short clips (< 50 ms)";
    }
    ImGui::SameLine();
    if (ImGui::Button("Find Clipped")) {
        m_qualityResults = m_manager.FindClipped();
        m_qualityResultsTitle = "Clipped audio (peak >= 0.999)";
    }

    if (!m_qualityResultsTitle.empty()) {
        ImGui::Text("%s: %zu", m_qualityResultsTitle.c_str(), m_qualityResults.size());
        ImGui::BeginChild("quality_results", ImVec2(0.0f, 90.0f), true);
        for (const std::string& id : m_qualityResults) {
            if (ImGui::Selectable(id.c_str(), id == m_selectedId)) {
                m_multiSelect.clear();
                m_multiSelect.insert(id);
                SelectEvent(id);
            }
        }
        ImGui::EndChild();
    }
}

void DatasetStudioPanel::RenderReplayQueueSection() {
    ImGui::SeparatorText("Replay Queue");
    if (m_replayQueue.empty()) {
        ImGui::TextDisabled("No recently played events");
        return;
    }
    for (const std::string& id : m_replayQueue) {
        ImGui::PushID(id.c_str());
        if (ImGui::SmallButton("Load")) {
            m_multiSelect.clear();
            m_multiSelect.insert(id);
            SelectEvent(id);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Replay")) {
            m_multiSelect.clear();
            m_multiSelect.insert(id);
            SelectEvent(id);
            const auto ev = m_manager.GetEvent(id);
            if (ev.has_value()) {
                ReplayAudioPath(ev->audioPath);
                m_isPlaying = true;
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(id.c_str());
        ImGui::PopID();
    }
}

void DatasetStudioPanel::RenderSessionSection() {
    ImGui::SeparatorText("Today's Progress");
    const auto stats = m_manager.GetStatistics();
    ImGui::Text("Labeled: %d", m_sessionLabeled);
    ImGui::Text("Moved: %d", m_sessionMoved);
    ImGui::Text("Deleted: %d", m_sessionDeleted);
    ImGui::Text("Undo: %d", m_sessionUndone);
    ImGui::Text("Remaining Unknown: %zu", stats.at("unknown"));
}

void DatasetStudioPanel::RenderDeleteConfirmPopup() {
    if (ImGui::BeginPopupModal("Delete Event?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete selected event(s)?");
        ImGui::TextDisabled("Deletion is soft-delete into dataset/.trash and can be undone.");
        if (ImGui::Button("YES", ImVec2(120.0f, 0.0f))) {
            DeleteSelection();
            m_confirmDeletePending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("NO", ImVec2(120.0f, 0.0f))) {
            m_confirmDeletePending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DatasetStudioPanel::RenderDetailPanel() {
    ImGui::BeginChild("dataset_detail", ImVec2(0.0f, 0.0f), false);
    if (m_previewDirty) {
        LoadPreviewForSelected();
    }

    if (m_selectedId.empty()) {
        ImGui::TextDisabled("Select an event from the left browser.");
        ImGui::EndChild();
        return;
    }

    RenderMetadataSection();
    RenderWaveformSection();
    RenderSpectrogramSection();
    RenderFeatureSection();
    RenderStatisticsSection();
    RenderQualitySection();
    RenderReplayQueueSection();
    RenderSessionSection();
    ImGui::EndChild();
}

void DatasetStudioPanel::Render() {
    HandleShortcuts();
    RecomputeFilteredList();

    ImGui::SetNextWindowSize(ImVec2(1500, 920), ImGuiCond_FirstUseEver);
    ImGui::Begin("EchoRadar Dataset Studio");
    RenderToolbar();

    ImGui::Columns(2, nullptr, true);
    RenderBrowser();
    ImGui::NextColumn();
    RenderDetailPanel();
    ImGui::Columns(1);
    RenderDeleteConfirmPopup();
    ImGui::End();
}
