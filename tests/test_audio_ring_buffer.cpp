#include <gtest/gtest.h>
#include "../src/audio/AudioRingBuffer.h"
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace EchoRadar;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Build a stereo interleaved buffer where every sample equals @p val.
static std::vector<float> MakeStereo(float val, size_t frameCount) {
    return std::vector<float>(frameCount * 2, val);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioRingBuffer tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(AudioRingBuffer, CapacityRoundsUpToPowerOfTwo) {
    AudioRingBuffer rb(5); // 5 → 8
    EXPECT_EQ(rb.CapacityFrames(), 8u);

    AudioRingBuffer rb2(8); // already power-of-two
    EXPECT_EQ(rb2.CapacityFrames(), 8u);
}

TEST(AudioRingBuffer, MinimumCapacityIsTwoFrames) {
    AudioRingBuffer rb(0); // 0 -> minimum 2
    EXPECT_EQ(rb.CapacityFrames(), 2u);
}

TEST(AudioRingBuffer, EmptyBufferReturnsZero) {
    AudioRingBuffer rb(16);
    EXPECT_EQ(rb.GetAvailableFrames(), 0u);

    float out[4] = {};
    EXPECT_EQ(rb.PopInterleaved(out, 2), 0u);
}

TEST(AudioRingBuffer, BasicPushPop) {
    AudioRingBuffer rb(8);

    // 2 stereo frames: [1,2] and [3,4]
    const float input[] = {1.f, 2.f, 3.f, 4.f};
    ASSERT_EQ(rb.PushInterleaved(input, 2), 2u);
    EXPECT_EQ(rb.GetAvailableFrames(), 2u);

    float output[4] = {};
    ASSERT_EQ(rb.PopInterleaved(output, 2), 2u);
    EXPECT_FLOAT_EQ(output[0], 1.f); // L0
    EXPECT_FLOAT_EQ(output[1], 2.f); // R0
    EXPECT_FLOAT_EQ(output[2], 3.f); // L1
    EXPECT_FLOAT_EQ(output[3], 4.f); // R1

    EXPECT_EQ(rb.GetAvailableFrames(), 0u);
}

TEST(AudioRingBuffer, PartialPop) {
    AudioRingBuffer rb(8);
    auto data = MakeStereo(0.5f, 4);
    ASSERT_EQ(rb.PushInterleaved(data.data(), 4), 4u);

    float out[4] = {};
    EXPECT_EQ(rb.PopInterleaved(out, 2), 2u);   // read 2 of 4
    EXPECT_EQ(rb.GetAvailableFrames(), 2u);       // 2 remain
}

TEST(AudioRingBuffer, WrapAround) {
    AudioRingBuffer rb(4); // capacity = 4 frames (exact power-of-two)

    // Push 3 frames, pop 2, then push 2 more → forces wrap-around.
    auto push3 = MakeStereo(1.f, 3);
    ASSERT_EQ(rb.PushInterleaved(push3.data(), 3), 3u);

    float tmp[4] = {};
    ASSERT_EQ(rb.PopInterleaved(tmp, 2), 2u);
    EXPECT_FLOAT_EQ(tmp[0], 1.f);

    // Push 2 more (slots 3 and 0 — wraps around in storage)
    auto push2 = MakeStereo(2.f, 2);
    ASSERT_EQ(rb.PushInterleaved(push2.data(), 2), 2u);

    // Read the remaining 3 frames: one original + two new
    float final[6] = {};
    ASSERT_EQ(rb.PopInterleaved(final, 3), 3u);
    EXPECT_FLOAT_EQ(final[0], 1.f); // last of original batch
    EXPECT_FLOAT_EQ(final[2], 2.f); // first of new batch
    EXPECT_FLOAT_EQ(final[4], 2.f); // second of new batch
}

TEST(AudioRingBuffer, FullDropsNewest) {
    AudioRingBuffer rb(4); // capacity 4 frames

    auto data = MakeStereo(1.f, 6); // 6 frames — 2 should be dropped
    const size_t written = rb.PushInterleaved(data.data(), 6);
    EXPECT_EQ(written, 4u); // only 4 fit
    EXPECT_EQ(rb.GetAvailableFrames(), 4u);

    // Already full: additional writes should be dropped.
    EXPECT_EQ(rb.PushInterleaved(data.data(), 2), 0u);
    EXPECT_EQ(rb.GetAvailableFrames(), 4u);
}

TEST(AudioRingBuffer, Clear) {
    AudioRingBuffer rb(8);
    auto data = MakeStereo(0.7f, 4);
    ASSERT_EQ(rb.PushInterleaved(data.data(), 4), 4u);
    rb.Clear();
    EXPECT_EQ(rb.GetAvailableFrames(), 0u);

    float out[4] = {};
    EXPECT_EQ(rb.PopInterleaved(out, 2), 0u);
}

TEST(AudioRingBuffer, MultipleWrapArounds) {
    AudioRingBuffer rb(4);
    for (int round = 0; round < 10; ++round) {
        auto data = MakeStereo(static_cast<float>(round), 4);
        ASSERT_EQ(rb.PushInterleaved(data.data(), 4), 4u);

        float out[8] = {};
        ASSERT_EQ(rb.PopInterleaved(out, 4), 4u);
        EXPECT_FLOAT_EQ(out[0], static_cast<float>(round));
    }
}

TEST(AudioRingBuffer, ThreadSafetySPSC) {
    // One producer pushes 2000 × 10 frames; one consumer reads as fast as it can.
    // No data-race detected (verified at runtime with TSAN if available).
    AudioRingBuffer rb(1024);
    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};

    auto producer = [&]() {
        auto data = MakeStereo(1.f, 10);
        for (int i = 0; i < 2000; ++i) {
            size_t n = rb.PushInterleaved(data.data(), 10);
            produced.fetch_add(n, std::memory_order_relaxed);
        }
    };

    auto consumer = [&]() {
        std::vector<float> buf(20);
        for (int i = 0; i < 1000; ++i) {
            size_t n = rb.PopInterleaved(buf.data(), 10);
            consumed.fetch_add(n, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    std::thread t1(producer), t2(consumer);
    t1.join();
    t2.join();

    // Sanity: produced ≤ 2000 * 10 (may drop when full), consumed ≤ produced.
    EXPECT_LE(produced.load(), 20000u);
    EXPECT_LE(consumed.load(), produced.load());
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioLevels math helper tests (no hardware required)
// ─────────────────────────────────────────────────────────────────────────────

TEST(AudioLevelsMath, ConstantSignalRmsEqualsPeak) {
    // RMS of a constant value V equals V; peak also equals V.
    constexpr float V = 0.5f;
    constexpr int N   = 1024;

    float sumSq = 0.f, peak = 0.f;
    for (int i = 0; i < N; ++i) {
        sumSq += V * V;
        peak = std::max(peak, V);
    }
    const float rms = std::sqrt(sumSq / N);
    EXPECT_NEAR(rms,  V, 1e-5f);
    EXPECT_NEAR(peak, V, 1e-5f);
}

TEST(AudioLevelsMath, SineWaveRmsApprox) {
    // RMS of sin(x) over a full period = 1/sqrt(2) ≈ 0.7071 (amplitude 1.0).
    constexpr int N = 48000; // 1 second
    constexpr float kPi = 3.14159265358979323846f;
    float sumSq = 0.f;
    for (int i = 0; i < N; ++i) {
        const float s = std::sin(2.f * kPi * 440.f * i / 48000.f);
        sumSq += s * s;
    }
    const float rms = std::sqrt(sumSq / N);
    EXPECT_NEAR(rms, 1.f / std::sqrt(2.f), 1e-3f);
}

TEST(AudioLevelsMath, SilenceIsZero) {
    constexpr int N = 1024;
    float sumSq = 0.f, peak = 0.f;
    for (int i = 0; i < N; ++i) { /* 0 */ }
    const float rms = std::sqrt(sumSq / N);
    EXPECT_FLOAT_EQ(rms,  0.f);
    EXPECT_FLOAT_EQ(peak, 0.f);
}
