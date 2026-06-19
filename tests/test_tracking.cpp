#include <gtest/gtest.h>
#include "../src/tracking/DirectionTracker.h"

using namespace EchoRadar;

static DirectionEstimate MakeEst(float angle, float conf = 0.8f) {
    DirectionEstimate e;
    e.angle = angle; e.confidence = conf;
    return e;
}

TEST(DirectionTracker, ConstructDefault) {
    EXPECT_NO_THROW(DirectionTracker t);
}

TEST(DirectionTracker, FirstUpdateSetsAngle) {
    DirectionTracker t;
    auto out = t.Update(MakeEst(120.0f));
    EXPECT_NEAR(out.angle, 120.0f, 1.0f);
}

TEST(DirectionTracker, SmoothesNoise) {
    DirectionTracker t;
    t.Update(MakeEst(120.0f));
    t.Update(MakeEst(126.0f));
    t.Update(MakeEst(122.0f));
    auto out = t.Update(MakeEst(125.0f));
    // Result should be between 120 and 126
    EXPECT_GE(out.angle, 120.0f);
    EXPECT_LE(out.angle, 126.0f);
}

TEST(DirectionTracker, WrapAround) {
    DirectionTracker t;
    t.Update(MakeEst(355.0f));
    auto out = t.Update(MakeEst(5.0f)); // crosses 0°
    // Should not jump to 180° mid-range
    EXPECT_TRUE(out.angle > 340.0f || out.angle < 20.0f);
}

TEST(DirectionTracker, Reset) {
    DirectionTracker t;
    t.Update(MakeEst(90.0f));
    t.Reset();
    EXPECT_FALSE(t.IsInitialised());
}
