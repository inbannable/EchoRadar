#include <gtest/gtest.h>
#include "../src/events/GunshotDetector.h"
#include "../src/events/FootstepDetector.h"
#include "../src/dsp/STFTProcessor.h"

using namespace EchoRadar;

static Spectrogram MakeEmptySpec() {
    Spectrogram s;
    s.fft_size = 1024;
    s.magnitude.push_back(std::vector<float>(513, 0.0f));
    return s;
}

TEST(GunshotDetector, ConstructDefault) {
    EXPECT_NO_THROW(GunshotDetector d);
}

TEST(GunshotDetector, ProcessSilence) {
    GunshotDetector d;
    bool fired = false;
    d.Process(MakeEmptySpec(), [&](const GunshotEvent&){ fired = true; });
    EXPECT_FALSE(fired); // stub: never fires on silence
}

TEST(GunshotDetector, Reset) {
    GunshotDetector d;
    d.Reset();
    EXPECT_FLOAT_EQ(d.LastEvent().confidence, 0.0f);
}

TEST(FootstepDetector, ConstructDefault) {
    EXPECT_NO_THROW(FootstepDetector d);
}

TEST(FootstepDetector, ProcessSilence) {
    FootstepDetector d;
    bool fired = false;
    d.Process(MakeEmptySpec(), [&](const FootstepEvent&){ fired = true; });
    EXPECT_FALSE(fired);
}
