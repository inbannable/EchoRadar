#include <gtest/gtest.h>
#include "../src/dsp/RingBuffer.h"
#include <thread>
#include <atomic>

using namespace EchoRadar;

static AudioFrame MakeFrame(float val, uint32_t n = 480) {
    AudioFrame f;
    f.left .assign(n, val);
    f.right.assign(n, val);
    f.sample_rate = 48000;
    return f;
}

TEST(RingBuffer, PushPop) {
    RingBuffer rb(4);
    rb.Push(MakeFrame(1.0f));
    rb.Push(MakeFrame(2.0f));

    AudioFrame out;
    ASSERT_TRUE(rb.Pop(out));
    EXPECT_FLOAT_EQ(out.left[0], 1.0f);

    ASSERT_TRUE(rb.Pop(out));
    EXPECT_FLOAT_EQ(out.left[0], 2.0f);

    EXPECT_FALSE(rb.Pop(out));
}

TEST(RingBuffer, Overflow) {
    RingBuffer rb(2);
    rb.Push(MakeFrame(1.0f));
    rb.Push(MakeFrame(2.0f));
    rb.Push(MakeFrame(3.0f)); // evicts frame 1

    AudioFrame out;
    ASSERT_TRUE(rb.Pop(out));
    EXPECT_FLOAT_EQ(out.left[0], 2.0f); // oldest is now 2
}

TEST(RingBuffer, GetLast100ms) {
    RingBuffer rb(256);
    // Each frame = 480 samples @ 48 kHz → 10 ms
    for (int i = 0; i < 20; ++i) rb.Push(MakeFrame(static_cast<float>(i)));
    auto frames = rb.GetLast100ms();
    EXPECT_GE(frames.size(), 9u);  // at least 90 ms
    EXPECT_LE(frames.size(), 11u); // at most 110 ms
}

TEST(RingBuffer, ThreadSafety) {
    RingBuffer rb(512);
    std::atomic<int> produced{0}, consumed{0};

    auto producer = [&]() {
        for (int i = 0; i < 1000; ++i) {
            rb.Push(MakeFrame(static_cast<float>(i)));
            ++produced;
        }
    };
    auto consumer = [&]() {
        AudioFrame f;
        for (int i = 0; i < 1000; ++i) {
            if (rb.Pop(f)) ++consumed;
        }
    };

    std::thread t1(producer), t2(consumer);
    t1.join(); t2.join();
    EXPECT_EQ(produced.load(), 1000);
}
