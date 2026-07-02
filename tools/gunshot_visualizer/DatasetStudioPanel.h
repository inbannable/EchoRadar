#pragma once

// EchoRadar Dataset Studio panel: turns gunshot_visualizer into a full
// dataset browsing/labeling/QA tool built on top of DatasetManager. All
// filesystem mutations (move/delete/restore/notes) go through
// EchoRadar::DatasetManager -- this class only renders ImGui and manages
// UI-local state (selection, search text, loaded preview buffers).

#include "dataset/DatasetManager.h"

#include <deque>
#include <set>
#include <string>
#include <vector>

class DatasetStudioPanel {
public:
    explicit DatasetStudioPanel(std::string datasetRoot);

    // Renders the whole studio window. Call once per frame.
    void Render();

private:
    void RefreshIndex();
    void RecomputeFilteredList();
    void SelectEvent(const std::string& id);
    void LoadPreviewForSelected();
    void ApplyLabelToSelection(EchoRadar::DatasetLabel label);
    void DeleteSelection();
    void RunUndo();
    void HandleShortcuts();
    void PushReplay(const std::string& id);
    void PlaySelectedAudio();
    void StopAudio();
    void ToastStatus(const std::string& message, bool ok);

    void RenderToolbar();
    void RenderBrowser();
    void RenderDetailPanel();
    void RenderWaveformSection();
    void RenderSpectrogramSection();
    void RenderFeatureSection();
    void RenderMetadataSection();
    void RenderStatisticsSection();
    void RenderQualitySection();
    void RenderReplayQueueSection();
    void RenderSessionSection();
    void RenderDeleteConfirmPopup();

    EchoRadar::DatasetManager m_manager;
    std::vector<EchoRadar::DatasetEventRecord> m_filtered;

    std::string m_selectedId;
    std::set<std::string> m_multiSelect;
    std::string m_lastAnchorId; // for shift-click range select

    std::string m_searchText;
    int m_labelFilterIndex{0}; // 0 = All, 1..6 = specific label
    float m_minConfidenceFilter{0.0f};
    float m_minScoreFilter{0.0f};

    // Loaded preview data for the currently selected event.
    std::vector<float> m_waveformLeft;
    std::vector<float> m_waveformRight;
    int m_specFrames{0};
    int m_specBins{0};
    float m_specMin{0.0f};
    float m_specMax{1.0f};
    std::vector<float> m_spectrogramLog;

    std::vector<std::string> m_featureColumnNames;
    std::vector<std::vector<float>> m_featureColumns;

    char m_notesBuffer[1024]{};
    bool m_previewDirty{true};
    double m_audioDurationSec{0.0};
    float m_waveformZoom{1.0f};
    float m_waveformOffset{0.0f};
    float m_spectrogramZoom{1.0f};
    float m_spectrogramOffset{0.0f};

    std::deque<std::string> m_replayQueue;
    static constexpr size_t kReplayQueueMax = 10;

    // Session progress counters (reset when the app restarts).
    int m_sessionLabeled{0};
    int m_sessionDeleted{0};
    int m_sessionMoved{0};
    int m_sessionUndone{0};

    std::string m_qualityResultsTitle;
    std::vector<std::string> m_qualityResults;

    bool m_confirmDeletePending{false};

    std::string m_statusMessage;
    bool m_statusOk{true};

    bool m_isPlaying{false};
};
