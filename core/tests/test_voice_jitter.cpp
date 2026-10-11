// SPDX-License-Identifier: GPL-3.0-or-later
#include <thread>
#include <vector>

#include "sdrstar/voice_jitter.h"
#include "test_framework.h"

using namespace sdrstar;

namespace {
struct FakeClock {
    double t = 0.0;
    VoiceJitterBuffer::Clock fn() { return [this] { return t; }; }
};

std::vector<int16_t> ramp(size_t frames, int channels, int16_t start = 1000) {
    std::vector<int16_t> v(frames * (size_t)channels);
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) { v[i * (size_t)channels + (size_t)c] = (int16_t)(start + (int)i + c * 5000); }
    }
    return v;
}
}  // namespace

TEST(idle_buffer_produces_nothing) {
    FakeClock clk;
    VoiceJitterBuffer jb({}, clk.fn());
    StereoFrame out[160];
    CHECK_EQ(jb.takeReady(out, 160), (size_t)0);
    CHECK(!jb.pending());
    CHECK(!jb.playing());
}

TEST(waits_for_the_prebuffer_before_playing) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());   // 100 ms = 800 frames
    auto a = ramp(400, 2);
    jb.push(a.data(), 400, 2);
    StereoFrame out[160];
    CHECK_EQ(jb.takeReady(out, 160), (size_t)0);   // only 50 ms queued
    CHECK(jb.pending());
    CHECK(!jb.playing());
    auto b = ramp(400, 2, 5000);
    jb.push(b.data(), 400, 2);                      // now 100 ms
    CHECK_EQ(jb.takeReady(out, 160), (size_t)160);
    CHECK(jb.playing());
    // the data comes out in order, converted to float
    CHECK_NEAR(out[0].l, 1000 / 32768.0, 1e-6);
    CHECK_NEAR(out[0].r, 6000 / 32768.0, 1e-6);
    CHECK_NEAR(out[159].l, 1159 / 32768.0, 1e-6);
}

TEST(a_short_transmission_still_plays_after_the_max_wait) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    auto a = ramp(160, 2);
    jb.push(a.data(), 160, 2);                      // 20 ms: never reaches the prebuffer
    StereoFrame out[160];
    clk.t = 0.100;
    CHECK_EQ(jb.takeReady(out, 160), (size_t)0);
    clk.t = 0.250;                                  // waited longer than maxWait
    CHECK_EQ(jb.takeReady(out, 160), (size_t)160);
    CHECK(!jb.pending());
    CHECK(!jb.playing());                           // drained: back to waiting
}

TEST(plays_through_and_returns_to_waiting_when_drained) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    auto a = ramp(1000, 1);
    jb.push(a.data(), 1000, 1);
    StereoFrame out[400];
    size_t total = 0, n;
    while ((n = jb.takeReady(out, 400)) > 0) { total += n; }
    CHECK_EQ(total, (size_t)1000);
    CHECK(!jb.playing());
    CHECK_EQ(jb.takeReady(out, 400), (size_t)0);
    // mono input is duplicated to both channels
    auto b = ramp(800, 1, 200);
    jb.push(b.data(), 800, 1);
    CHECK_EQ(jb.takeReady(out, 400), (size_t)400);
    CHECK_NEAR(out[0].l, out[0].r, 1e-9);
}

TEST(underrun_mid_call_re_buffers_instead_of_stuttering) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    auto a = ramp(900, 2);
    jb.push(a.data(), 900, 2);
    StereoFrame out[900];
    CHECK_EQ(jb.takeReady(out, 900), (size_t)900);  // plays everything it has
    CHECK(!jb.playing());
    // A small trickle arrives: it is held back until a proper prebuffer exists again.
    auto b = ramp(160, 2);
    clk.t = 1.0;
    jb.push(b.data(), 160, 2);
    CHECK_EQ(jb.takeReady(out, 900), (size_t)0);
    auto c = ramp(700, 2);
    jb.push(c.data(), 700, 2);
    CHECK_EQ(jb.takeReady(out, 900), (size_t)860);
}

TEST(queue_is_bounded_and_keeps_the_newest_audio) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 1.0}, clk.fn());   // 1 s cap = 8000 frames
    for (int i = 0; i < 5; ++i) {
        auto a = ramp(4000, 2, (int16_t)(i * 1000));
        jb.push(a.data(), 4000, 2);
    }
    CHECK_EQ(jb.queuedFrames(), (size_t)8000);
    std::vector<StereoFrame> out(8000);
    CHECK_EQ(jb.takeReady(out.data(), 8000), (size_t)8000);
    // the last push (start value 4000) must be the tail of what is left
    CHECK_NEAR(out[7999].l, (4000 + 3999) / 32768.0, 1e-6);
}

TEST(a_call_on_one_tdma_slot_is_played_on_both_sides) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    std::vector<int16_t> slot1(1000 * 2, 0), slot2(1000 * 2, 0);
    for (size_t i = 0; i < 1000; ++i) {
        slot1[i * 2] = (int16_t)(1000 + (int)i);   // DMR slot 1 = left channel, right silent
        slot2[i * 2 + 1] = (int16_t)(2000 + (int)i);   // slot 2 = right channel, left silent
    }
    jb.push(slot1.data(), 1000, 2);
    jb.push(slot2.data(), 1000, 2);
    std::vector<StereoFrame> out(2000);
    CHECK_EQ(jb.takeReady(out.data(), 2000), (size_t)2000);
    CHECK_NEAR(out[10].l, 1010 / 32768.0, 1e-6);
    CHECK_NEAR(out[10].r, 1010 / 32768.0, 1e-6);       // duplicated, not left-only
    CHECK_NEAR(out[1010].l, 2010 / 32768.0, 1e-6);
    CHECK_NEAR(out[1010].r, 2010 / 32768.0, 1e-6);
}

TEST(two_simultaneous_calls_stay_separated_left_and_right) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    std::vector<int16_t> both(1000 * 2);
    for (size_t i = 0; i < 1000; ++i) {
        both[i * 2] = (int16_t)(1000 + (int)i);
        both[i * 2 + 1] = (int16_t)(-3000 - (int)i);
    }
    jb.push(both.data(), 1000, 2);
    std::vector<StereoFrame> out(1000);
    CHECK_EQ(jb.takeReady(out.data(), 1000), (size_t)1000);
    CHECK_NEAR(out[5].l, 1005 / 32768.0, 1e-6);
    CHECK_NEAR(out[5].r, -3005 / 32768.0, 1e-6);
}

TEST(silence_stays_silence) {
    FakeClock clk;
    VoiceJitterBuffer jb({8000, 100.0, 240.0, 3.0}, clk.fn());
    std::vector<int16_t> zeros(1000 * 2, 0);
    jb.push(zeros.data(), 1000, 2);
    std::vector<StereoFrame> out(1000);
    CHECK_EQ(jb.takeReady(out.data(), 1000), (size_t)1000);
    CHECK(out[3].l == 0.0f && out[3].r == 0.0f);
}

TEST(clear_drops_everything) {
    FakeClock clk;
    VoiceJitterBuffer jb({}, clk.fn());
    auto a = ramp(2000, 2);
    jb.push(a.data(), 2000, 2);
    StereoFrame out[100];
    CHECK_EQ(jb.takeReady(out, 100), (size_t)100);
    CHECK(jb.playing());
    jb.clear();
    CHECK(!jb.pending());
    CHECK(!jb.playing());
    CHECK_EQ(jb.takeReady(out, 100), (size_t)0);
}

TEST(bad_input_is_ignored) {
    FakeClock clk;
    VoiceJitterBuffer jb({}, clk.fn());
    jb.push(nullptr, 100, 2);
    int16_t x[4] = {1, 2, 3, 4};
    jb.push(x, 0, 2);
    jb.push(x, 2, 0);
    CHECK(!jb.pending());
}

TEST(concurrent_push_and_take_never_loses_or_reorders) {
    // The decoder thread pushes while the feeder thread takes (real clock).
    VoiceJitterBuffer jb({8000, 5.0, 20.0, 10.0});   // capacity > everything sent: nothing may be dropped
    const size_t kFrames = 40000;
    std::thread producer([&] {
        std::vector<int16_t> chunk(160 * 2);
        for (size_t sent = 0; sent < kFrames; sent += 160) {
            for (size_t i = 0; i < 160; ++i) {
                chunk[i * 2] = (int16_t)((sent + i) % 30000);
                chunk[i * 2 + 1] = chunk[i * 2];
            }
            jb.push(chunk.data(), 160, 2);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    size_t got = 0;
    bool ordered = true;
    std::vector<StereoFrame> out(512);
    const auto t0 = std::chrono::steady_clock::now();
    while (got < kFrames && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
        size_t n = jb.takeReady(out.data(), out.size());
        for (size_t i = 0; i < n; ++i) {
            const float want = (float)((got + i) % 30000) / 32768.0f;
            if (std::fabs(out[i].l - want) > 1e-6f) { ordered = false; }
        }
        got += n;
        if (n == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
    }
    producer.join();
    CHECK_EQ(got, kFrames);
    CHECK(ordered);
}

TEST_MAIN("voice_jitter")
