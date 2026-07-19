#include "dataset/AssetInventory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace EchoRadar;

namespace {

void WritePcmWav(const fs::path& path,
                 uint32_t sampleRate,
                 uint16_t channels,
                 uint16_t bitDepth,
                 uint32_t frameCount) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const uint16_t bytesPerSample = static_cast<uint16_t>(bitDepth / 8u);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bytesPerSample);
    const uint32_t dataBytes = frameCount * blockAlign;
    const uint32_t riffSize = 36u + dataBytes;
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t fmtSize = 16;
    const uint16_t pcmFormat = 1;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&pcmFormat), sizeof(pcmFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitDepth), sizeof(bitDepth));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), sizeof(dataBytes));

    const size_t sampleCount = static_cast<size_t>(frameCount) * channels;
    for (size_t i = 0; i < sampleCount; ++i) {
        if (bitDepth == 8) {
            const uint8_t sample = (i % 2 == 0) ? 192u : 64u;
            out.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
        } else {
            const int16_t sample = (i % 2 == 0) ? 16384 : -16384;
            out.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
        }
    }
}

const AssetRecord* FindRecord(const std::vector<AssetRecord>& records, const std::string& path) {
    const auto it = std::find_if(records.begin(), records.end(), [&](const AssetRecord& record) {
        return record.relativePath == path;
    });
    return it == records.end() ? nullptr : &*it;
}

class AssetInventoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
            ("echoradar_asset_inventory_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override {
        fs::remove_all(root);
    }

    fs::path root;
};

TEST_F(AssetInventoryTest, ComputesStandardSha256) {
    const fs::path path = root / "abc.txt";
    std::ofstream(path, std::ios::binary) << "abc";
    bool ok = false;
    EXPECT_EQ(AssetInventory::ComputeFileSha256(path, &ok),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(ok);
}

TEST_F(AssetInventoryTest, ReadsPcm8AndPcm16WavHeadersAndLevels) {
    const fs::path pcm8 = root / "pcm8.wav";
    const fs::path pcm16 = root / "pcm16.wav";
    WritePcmWav(pcm8, 22050, 1, 8, 2205);
    WritePcmWav(pcm16, 44100, 2, 16, 4410);

    const AssetAudioInfo first = AssetInventory::ReadPcmWavInfo(pcm8);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_EQ(first.sampleRate, 22050u);
    EXPECT_EQ(first.channels, 1u);
    EXPECT_EQ(first.bitDepth, 8u);
    EXPECT_NEAR(first.durationMs, 100.0, 0.01);
    EXPECT_NEAR(first.peak, 0.5f, 0.01f);

    const AssetAudioInfo second = AssetInventory::ReadPcmWavInfo(pcm16);
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(second.sampleRate, 44100u);
    EXPECT_EQ(second.channels, 2u);
    EXPECT_EQ(second.bitDepth, 16u);
    EXPECT_NEAR(second.durationMs, 100.0, 0.01);
    EXPECT_NEAR(second.peak, 0.5f, 0.01f);
}

TEST_F(AssetInventoryTest, ClassifiesAssetsIgnoresMacMetadataAndFindsDuplicates) {
    WritePcmWav(root / "player/footsteps/concrete_ct_01.wav", 44100, 1, 16, 4410);
    fs::create_directories(root / "player/footsteps");
    fs::copy_file(root / "player/footsteps/concrete_ct_01.wav",
                  root / "player/footsteps/concrete_ct_02.wav");
    WritePcmWav(root / "weapons/ak47/ak47_01.wav", 44100, 1, 16, 4411);
    WritePcmWav(root / "weapons/ak47/ak47_clipin.wav", 44100, 1, 16, 2205);
    WritePcmWav(root / "weapons/fx/rics/bullet_ric_01.wav", 44100, 2, 16, 2206);
    WritePcmWav(root / "weapons/negev/negev_clean_01.wav", 44100, 1, 16, 4412);
    WritePcmWav(root / "player/damage1.wav", 22050, 1, 8, 2207);
    std::ofstream(root / "player/._damage1.wav", std::ios::binary) << "AppleDouble";
    std::ofstream(root / ".DS_Store", std::ios::binary) << "metadata";
    std::ofstream(root / "notes.txt") << "not audio";

    AssetInventory inventory(root);
    std::string error;
    ASSERT_TRUE(inventory.Scan(&error)) << error;
    const AssetInventorySummary& summary = inventory.GetSummary();
    EXPECT_EQ(summary.discoveredFiles, 10u);
    EXPECT_EQ(summary.ignoredFiles, 3u);
    EXPECT_EQ(summary.validWavFiles, 7u);
    EXPECT_EQ(summary.invalidWavFiles, 0u);
    EXPECT_EQ(summary.duplicateFiles, 1u);
    EXPECT_EQ(summary.gunshots, 2u);
    EXPECT_EQ(summary.footsteps, 2u);
    EXPECT_EQ(summary.mechanical, 1u);
    EXPECT_EQ(summary.other, 2u);
    EXPECT_EQ(summary.reviewNeeded, 1u);

    const auto& records = inventory.GetRecords();
    const AssetRecord* footstep = FindRecord(records, "player/footsteps/concrete_ct_01.wav");
    ASSERT_NE(footstep, nullptr);
    EXPECT_EQ(footstep->label, AssetLabel::Footstep);
    EXPECT_EQ(footstep->surface, "concrete");

    const AssetRecord* shot = FindRecord(records, "weapons/ak47/ak47_01.wav");
    ASSERT_NE(shot, nullptr);
    EXPECT_EQ(shot->label, AssetLabel::Gunshot);
    EXPECT_EQ(shot->weapon, "ak47");
    EXPECT_EQ(shot->distance, "near");

    const AssetRecord* mechanical = FindRecord(records, "weapons/ak47/ak47_clipin.wav");
    ASSERT_NE(mechanical, nullptr);
    EXPECT_EQ(mechanical->label, AssetLabel::Mechanical);
    EXPECT_EQ(mechanical->subtype, "reload");

    const AssetRecord* duplicate = FindRecord(records, "player/footsteps/concrete_ct_02.wav");
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->duplicateOf, "player/footsteps/concrete_ct_01.wav");

    const fs::path reports = root / "reports";
    ASSERT_TRUE(inventory.Export(reports, &error)) << error;
    EXPECT_TRUE(fs::exists(reports / "asset_manifest.csv"));
    EXPECT_TRUE(fs::exists(reports / "review_needed.csv"));
    EXPECT_TRUE(fs::exists(reports / "summary.json"));

    std::ifstream review(reports / "review_needed.csv");
    const std::string reviewText((std::istreambuf_iterator<char>(review)), std::istreambuf_iterator<char>());
    EXPECT_NE(reviewText.find("weapons/negev/negev_clean_01.wav"), std::string::npos);
    EXPECT_EQ(reviewText.find("weapons/ak47/ak47_01.wav"), std::string::npos);
}

} // namespace
