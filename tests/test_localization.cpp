#include <gtest/gtest.h>
#include "../src/localization/KNNDirectionEstimator.h"

using namespace EchoRadar;

static FeatureVector MakeFV(float ild = 0, float itd = 0) {
    FeatureVector fv;
    fv.ild = ild; fv.itd = itd;
    fv.energy_bands.assign(8, 0.0f);
    return fv;
}

TEST(KNNDirectionEstimator, ConstructDefault) {
    EXPECT_NO_THROW(KNNDirectionEstimator e);
}

TEST(KNNDirectionEstimator, InvalidK) {
    KNNDirectionEstimator::Config cfg;
    cfg.k = 0;
    EXPECT_THROW(KNNDirectionEstimator e(cfg), std::invalid_argument);
}

TEST(KNNDirectionEstimator, EstimateEmpty) {
    KNNDirectionEstimator e;
    auto result = e.Estimate(MakeFV());
    EXPECT_FLOAT_EQ(result.angle, 0.0f);
    EXPECT_FLOAT_EQ(result.confidence, 0.0f);
}

TEST(KNNDirectionEstimator, AddAndEstimate) {
    KNNDirectionEstimator e;
    e.AddSample(MakeFV(3.0f, 10.0f), 90.0f);
    e.AddSample(MakeFV(-3.0f, -10.0f), 270.0f);
    EXPECT_EQ(e.SampleCount(), 2u);

    // Query close to first sample
    auto result = e.Estimate(MakeFV(3.1f, 10.1f));
    EXPECT_NEAR(result.angle, 90.0f, 1.0f);
}
