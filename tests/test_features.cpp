#include <gtest/gtest.h>
#include "../src/features/FeatureExtractor.h"

using namespace EchoRadar;

static Spectrogram MakeSpec(std::size_t frames = 1, std::size_t bins = 513) {
    Spectrogram s;
    s.fft_size = 1024;
    for (std::size_t i = 0; i < frames; ++i)
        s.magnitude.push_back(std::vector<float>(bins, 0.0f));
    return s;
}

TEST(FeatureExtractor, ConstructDefault) {
    EXPECT_NO_THROW(FeatureExtractor fe);
}

TEST(FeatureExtractor, InvalidBands) {
    FeatureExtractor::Config cfg;
    cfg.n_energy_bands = 0;
    EXPECT_THROW(FeatureExtractor fe(cfg), std::invalid_argument);
}

TEST(FeatureExtractor, ExtractFromEmpty) {
    FeatureExtractor fe;
    Spectrogram empty;
    auto fv = fe.Extract(empty, empty, 0);
    EXPECT_FLOAT_EQ(fv.ild, 0.0f);
    EXPECT_FLOAT_EQ(fv.itd, 0.0f);
}

TEST(FeatureExtractor, ExtractSilence) {
    FeatureExtractor fe;
    auto spec = MakeSpec();
    auto fv = fe.Extract(spec, spec, 100);
    EXPECT_FLOAT_EQ(fv.ild, 0.0f);
    EXPECT_EQ(fv.energy_bands.size(), 8u);
    EXPECT_EQ(fv.timestamp, 100u);
}
