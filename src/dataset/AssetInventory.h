#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace EchoRadar {

enum class AssetLabel {
    Gunshot,
    Footstep,
    Mechanical,
    Other,
};

const char* ToString(AssetLabel label);

struct AssetAudioInfo {
    bool ok{false};
    uint32_t sampleRate{0};
    uint16_t channels{0};
    uint16_t bitDepth{0};
    uint64_t frameCount{0};
    double durationMs{0.0};
    float peak{0.0f};
    float rms{0.0f};
    std::string error;
};

struct AssetRecord {
    std::string assetId;
    std::string relativePath;
    std::string sha256;
    AssetLabel label{AssetLabel::Other};
    std::string subtype;
    std::string weapon;
    std::string surface;
    std::string distance;
    std::string sourceGroup;
    std::string classificationRule;
    float classificationConfidence{0.0f};
    AssetAudioInfo audio;
    std::string duplicateOf;
    bool included{false};
};

struct AssetInventorySummary {
    size_t discoveredFiles{0};
    size_t ignoredFiles{0};
    size_t validWavFiles{0};
    size_t invalidWavFiles{0};
    size_t duplicateFiles{0};
    size_t gunshots{0};
    size_t footsteps{0};
    size_t mechanical{0};
    size_t other{0};
    size_t reviewNeeded{0};
    double totalDurationSeconds{0.0};
};

class AssetInventory {
public:
    explicit AssetInventory(std::filesystem::path assetRoot);

    bool Scan(std::string* error = nullptr);
    bool Export(const std::filesystem::path& outputDirectory, std::string* error = nullptr) const;

    const std::filesystem::path& GetAssetRoot() const { return m_assetRoot; }
    const std::vector<AssetRecord>& GetRecords() const { return m_records; }
    const AssetInventorySummary& GetSummary() const { return m_summary; }

    static AssetAudioInfo ReadPcmWavInfo(const std::filesystem::path& path);
    static std::string ComputeFileSha256(const std::filesystem::path& path, bool* ok = nullptr);

private:
    std::filesystem::path m_assetRoot;
    std::vector<AssetRecord> m_records;
    AssetInventorySummary m_summary;
};

} // namespace EchoRadar
