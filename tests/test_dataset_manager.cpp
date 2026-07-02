#include "dataset/DatasetManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using namespace EchoRadar;

namespace {

void WriteWavPcm16(const fs::path& path, const std::vector<float>& interleaved, uint32_t sampleRate = 48000) {
    std::ofstream out(path, std::ios::binary);
    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
    const uint32_t dataBytes = static_cast<uint32_t>((interleaved.size() / 2) * blockAlign);
    const uint32_t riffSize = 36u + dataBytes;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    const uint32_t fmtChunkSize = 16;
    const uint16_t audioFormat = 1;
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), sizeof(dataBytes));
    for (float v : interleaved) {
        const int16_t pcm = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        out.write(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
    }
}

void WriteMetadata(const fs::path& path, const std::string& label, const std::string& id, float confidence = 0.5f) {
    std::ofstream out(path);
    out << "{\n";
    out << "  \"event_id\": \"" << id << "\",\n";
    out << "  \"event_type\": \"candidate\",\n";
    out << "  \"label\": \"" << label << "\",\n";
    out << "  \"timestamp_ms\": 1000,\n";
    out << "  \"sample_rate\": 48000,\n";
    out << "  \"fft_size\": 1024,\n";
    out << "  \"hop_size\": 512,\n";
    out << "  \"window_start_sample\": 0,\n";
    out << "  \"window_frames\": 19200,\n";
    out << "  \"feature_rows\": 35,\n";
    out << "  \"detector_score\": 0.9,\n";
    out << "  \"candidate_score\": 0.8,\n";
    out << "  \"confidence\": " << confidence << ",\n";
    out << "  \"trigger_threshold\": 0.56,\n";
    out << "  \"device_name\": \"Test Device\"\n";
    out << "}\n";
}

// Creates dataset/<label>/<id>/{audio.wav,features.csv,metadata.json} with a
// short synthetic (mostly-silent, low amplitude) stereo waveform.
void CreateEvent(const fs::path& root, const std::string& label, const std::string& id,
                  size_t frameCount = 4800, float amplitude = 0.1f, float confidence = 0.5f) {
    const fs::path dir = root / label / id;
    fs::create_directories(dir);
    std::vector<float> interleaved(frameCount * 2, 0.0f);
    for (size_t i = 0; i < frameCount; ++i) {
        interleaved[i * 2] = amplitude;
        interleaved[i * 2 + 1] = amplitude;
    }
    WriteWavPcm16(dir / "audio.wav", interleaved);
    std::ofstream csv(dir / "features.csv");
    csv << "time_sec,energy\n0.0,0.1\n";
    WriteMetadata(dir / "metadata.json", label, id, confidence);
}

class DatasetManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / ("echoradar_dataset_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override {
        fs::remove_all(root);
    }

    fs::path root;
};

TEST_F(DatasetManagerTest, ScanFindsAllEvents) {
    CreateEvent(root, "unknown", "000001");
    CreateEvent(root, "unknown", "000002");
    CreateEvent(root, "gunshot", "000003");

    DatasetManager mgr(root.string());
    mgr.Scan();

    EXPECT_EQ(mgr.GetEvents().size(), 3u);
    const auto stats = mgr.GetStatistics();
    EXPECT_EQ(stats.at("unknown"), 2u);
    EXPECT_EQ(stats.at("gunshot"), 1u);
}

TEST_F(DatasetManagerTest, MoveLabelMovesFolderAndUpdatesMetadata) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();

    const auto result = mgr.MoveLabel("000001", DatasetLabel::Gunshot);
    EXPECT_TRUE(result.ok);

    EXPECT_FALSE(fs::exists(root / "unknown" / "000001"));
    ASSERT_TRUE(fs::exists(root / "gunshot" / "000001"));

    const auto ev = mgr.GetEvent("000001");
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->label, DatasetLabel::Gunshot);

    // Re-scanning should reflect the same state on disk.
    DatasetManager mgr2(root.string());
    mgr2.Scan();
    const auto ev2 = mgr2.GetEvent("000001");
    ASSERT_TRUE(ev2.has_value());
    EXPECT_EQ(ev2->label, DatasetLabel::Gunshot);
}

TEST_F(DatasetManagerTest, MoveLabelIsNoOpWhenSameLabel) {
    CreateEvent(root, "gunshot", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();
    const auto result = mgr.MoveLabel("000001", DatasetLabel::Gunshot);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(fs::exists(root / "gunshot" / "000001"));
}

TEST_F(DatasetManagerTest, DeleteMovesToTrashAndRemovesFromIndex) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();

    const auto result = mgr.Delete("000001");
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(fs::exists(root / "unknown" / "000001"));
    EXPECT_TRUE(fs::exists(root / ".trash" / "000001"));
    EXPECT_FALSE(mgr.GetEvent("000001").has_value());
}

TEST_F(DatasetManagerTest, RestoreBringsBackDeletedEvent) {
    CreateEvent(root, "gunshot", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();
    mgr.Delete("000001");

    const auto result = mgr.Restore("000001");
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(fs::exists(root / "gunshot" / "000001"));
    const auto ev = mgr.GetEvent("000001");
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->label, DatasetLabel::Gunshot);
}

TEST_F(DatasetManagerTest, UndoRevertsMove) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();
    mgr.MoveLabel("000001", DatasetLabel::Footstep);
    ASSERT_TRUE(fs::exists(root / "footstep" / "000001"));

    const auto result = mgr.Undo();
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(fs::exists(root / "unknown" / "000001"));
    EXPECT_FALSE(fs::exists(root / "footstep" / "000001"));
}

TEST_F(DatasetManagerTest, UndoRevertsDelete) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();
    mgr.Delete("000001");
    ASSERT_FALSE(fs::exists(root / "unknown" / "000001"));

    const auto result = mgr.Undo();
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(fs::exists(root / "unknown" / "000001"));
    ASSERT_TRUE(mgr.GetEvent("000001").has_value());
}

TEST_F(DatasetManagerTest, UpdateNotesPersistsAndUndoRestoresOld) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();

    auto result = mgr.UpdateNotes("000001", "AK spray, clean sample");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(mgr.GetEvent("000001")->notes, "AK spray, clean sample");

    DatasetManager mgr2(root.string());
    mgr2.Scan();
    EXPECT_EQ(mgr2.GetEvent("000001")->notes, "AK spray, clean sample");

    mgr.Undo();
    EXPECT_EQ(mgr.GetEvent("000001")->notes, "");
}

TEST_F(DatasetManagerTest, UndoStackLimitedTo20) {
    CreateEvent(root, "unknown", "000001");
    DatasetManager mgr(root.string());
    mgr.Scan();

    // Bounce the label back and forth many times to exceed the undo limit.
    for (int i = 0; i < 25; ++i) {
        mgr.MoveLabel("000001", (i % 2 == 0) ? DatasetLabel::Gunshot : DatasetLabel::Unknown);
    }
    EXPECT_LE(mgr.UndoStackSize(), DatasetManager::kMaxUndoHistory);
}

TEST_F(DatasetManagerTest, FindVeryShortFlagsShortClips) {
    CreateEvent(root, "unknown", "000001", /*frameCount=*/100); // ~2ms @ 48kHz
    CreateEvent(root, "unknown", "000002", /*frameCount=*/19200); // 400ms
    DatasetManager mgr(root.string());
    mgr.Scan();

    const auto shortOnes = mgr.FindVeryShort(50.0);
    EXPECT_NE(std::find(shortOnes.begin(), shortOnes.end(), "000001"), shortOnes.end());
    EXPECT_EQ(std::find(shortOnes.begin(), shortOnes.end(), "000002"), shortOnes.end());
}

TEST_F(DatasetManagerTest, FindClippedFlagsFullScaleAmplitude) {
    CreateEvent(root, "unknown", "000001", 4800, /*amplitude=*/1.0f);
    CreateEvent(root, "unknown", "000002", 4800, /*amplitude=*/0.1f);
    DatasetManager mgr(root.string());
    mgr.Scan();

    const auto clipped = mgr.FindClipped(0.99f);
    EXPECT_NE(std::find(clipped.begin(), clipped.end(), "000001"), clipped.end());
    EXPECT_EQ(std::find(clipped.begin(), clipped.end(), "000002"), clipped.end());
}

TEST_F(DatasetManagerTest, FindDuplicatesDetectsByteIdenticalAudio) {
    CreateEvent(root, "unknown", "000001", 4800, 0.3f);
    CreateEvent(root, "gunshot", "000002", 4800, 0.3f);
    CreateEvent(root, "unknown", "000003", 4800, 0.7f);
    DatasetManager mgr(root.string());
    mgr.Scan();

    const auto dupes = mgr.FindDuplicates();
    EXPECT_NE(std::find(dupes.begin(), dupes.end(), "000001"), dupes.end());
    EXPECT_NE(std::find(dupes.begin(), dupes.end(), "000002"), dupes.end());
    EXPECT_EQ(std::find(dupes.begin(), dupes.end(), "000003"), dupes.end());
}

} // namespace
