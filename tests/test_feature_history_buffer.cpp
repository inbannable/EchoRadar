#include <gtest/gtest.h>
#include "../src/features/FeatureHistoryBuffer.h"

using namespace EchoRadar;

TEST(FeatureHistoryBuffer, EvictsEntriesOutsideHistoryWindow) {
    FeatureHistoryBuffer history(1.0);
    AudioFeatures f{};

    history.Push(f, 0.0, 0, 0.1f, 0.0f);
    history.Push(f, 0.5, 1, 0.2f, 0.0f);
    history.Push(f, 1.6, 2, 0.3f, 0.0f);

    EXPECT_EQ(history.Size(), 1u);
    EXPECT_NEAR(history.GetOldestTimeSec(), 1.6, 1e-6);
}

TEST(FeatureHistoryBuffer, ExtractWindowReturnsOnlyMatchingEntries) {
    FeatureHistoryBuffer history(3.0);
    AudioFeatures f{};

    history.Push(f, 0.2, 1, 0.1f, 0.0f);
    history.Push(f, 0.5, 2, 0.2f, 0.0f);
    history.Push(f, 0.8, 3, 0.3f, 0.1f);

    const auto slice = history.ExtractWindow(0.45, 0.85);
    ASSERT_EQ(slice.size(), 2u);
    EXPECT_EQ(slice[0].frameIndex, 2);
    EXPECT_EQ(slice[1].frameIndex, 3);
    EXPECT_FLOAT_EQ(slice[1].confidence, 0.1f);
}
