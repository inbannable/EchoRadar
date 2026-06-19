#include <gtest/gtest.h>
#include "../src/dsp/STFTProcessor.h"
#include <cmath>
#include <vector>

using namespace EchoRadar;

TEST(STFTProcessor, ConstructDefault) {
    EXPECT_NO_THROW(STFTProcessor p);
}

TEST(STFTProcessor, InvalidFFTSize) {
    STFTProcessor::Config cfg;
    cfg.fft_size = 1000; // not a power of 2
    EXPECT_THROW(STFTProcessor p(cfg), std::invalid_argument);
}

TEST(STFTProcessor, BinToHz) {
    STFTProcessor p;
    // Bin 0 → 0 Hz, bin 512 → Nyquist (24 kHz)
    EXPECT_FLOAT_EQ(p.BinToHz(0), 0.0f);
    EXPECT_NEAR(p.BinToHz(512), 24000.0f, 1.0f);
}

TEST(STFTProcessor, ProcessReturnsMagnitude) {
    STFTProcessor p;
    std::vector<float> silence(2048, 0.0f);
    auto spec = p.Process(silence);
    EXPECT_FALSE(spec.magnitude.empty());
    EXPECT_EQ(spec.magnitude[0].size(), 513u); // fft_size/2 + 1
}

TEST(STFTProcessor, ProcessStereo) {
    STFTProcessor p;
    AudioFrame frame;
    frame.left .assign(2048, 0.0f);
    frame.right.assign(2048, 0.0f);
    frame.sample_rate = 48000;
    auto [l, r] = p.ProcessStereo(frame);
    EXPECT_FALSE(l.magnitude.empty());
    EXPECT_FALSE(r.magnitude.empty());
}
