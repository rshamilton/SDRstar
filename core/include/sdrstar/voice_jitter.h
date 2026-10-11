// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceJitterBuffer: smooths the decoded-voice stream before it goes to the audio device.  DSD-neo hands over 20 ms voice
// frames about every 20-30 ms (sometimes several at once, sometimes a late one); an audio device wants a steady supply.
//
//  * Nothing is played until `prebufferMs` of audio is queued (or the oldest queued audio has waited `maxWaitMs`, so a
//    short transmission still comes out), so speech is not chopped up by gaps between bursts.
//  * Once playing, audio is handed out as fast as the consumer asks for it; when the queue runs dry the buffer goes
//    back to "waiting" instead of inventing silence, which means an idle voice stream produces NO audio data at all.
//  * The queue is bounded: when the decoder runs ahead of playback the oldest audio is dropped, never the caller blocked.
//
// Pure logic with an injectable clock so it is unit-tested without any audio hardware.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace sdrstar {

struct StereoFrame {
    float l = 0.0f;
    float r = 0.0f;
};

struct VoiceJitterConfig {
    int rate = 8000;               // frames per second of the queued audio
    double prebufferMs = 120.0;    // audio that must be queued before playback starts (DSD-neo delivers a 20 ms voice frame about every 20-30 ms)
    double maxWaitMs = 240.0;      // ...unless the oldest queued audio has already waited this long
    double capacitySec = 3.0;      // bound on the queue
};

class VoiceJitterBuffer {
public:
    using Config = VoiceJitterConfig;
    using Clock = std::function<double()>;   // seconds, monotonic

    VoiceJitterBuffer() : clock_(defaultClock()) {}
    explicit VoiceJitterBuffer(const Config& cfg, Clock clock = defaultClock()) : cfg_(cfg), clock_(std::move(clock)) {}

    // Thread-safe, never blocks.  `interleaved` holds frames*channels s16 samples (channels >= 1; extra channels ignored).
    // The two-slot modes (DMR, P25 Phase 2) put each call on ONE channel and leave the other digitally silent; played
    // as is, a single call would come out of one speaker only.  A block whose second channel is entirely silent (or the
    // first) is therefore played on both sides; when both slots carry audio they stay left / right.
    void push(const int16_t* interleaved, size_t frames, int channels) {
        if (!interleaved || frames == 0 || channels < 1) { return; }
        const size_t ch = (size_t)channels;
        bool leftSilent = true, rightSilent = true;
        if (channels > 1) {
            for (size_t i = 0; i < frames; ++i) {
                if (interleaved[i * ch] != 0) { leftSilent = false; }
                if (interleaved[i * ch + 1] != 0) { rightSilent = false; }
            }
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) { firstQueuedAt_ = clock_(); }
        for (size_t i = 0; i < frames; ++i) {
            StereoFrame f;
            f.l = interleaved[i * ch] / 32768.0f;
            f.r = channels > 1 ? interleaved[i * ch + 1] / 32768.0f : f.l;
            if (channels > 1 && !(leftSilent && rightSilent)) {
                if (rightSilent) { f.r = f.l; }
                if (leftSilent) { f.l = f.r; }
            }
            q_.push_back(f);
        }
        const size_t cap = (size_t)(cfg_.capacitySec * cfg_.rate);
        if (q_.size() > cap) { q_.erase(q_.begin(), q_.begin() + (long)(q_.size() - cap)); }   // the oldest audio is stale by then
    }

    // Take up to `maxFrames` frames that are ready to play NOW.  Returns 0 while still pre-buffering or when empty.
    size_t takeReady(StereoFrame* out, size_t maxFrames) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) {
            playing_ = false;
            return 0;
        }
        if (!playing_) {
            const double queuedMs = 1000.0 * (double)q_.size() / cfg_.rate;
            const double waitedMs = 1000.0 * (clock_() - firstQueuedAt_);
            if (queuedMs < cfg_.prebufferMs && waitedMs < cfg_.maxWaitMs) { return 0; }
            playing_ = true;
        }
        const size_t n = std::min(maxFrames, q_.size());
        std::copy(q_.begin(), q_.begin() + (long)n, out);
        q_.erase(q_.begin(), q_.begin() + (long)n);
        if (q_.empty()) { playing_ = false; }   // end of this burst/transmission: wait for the next one
        return n;
    }

    // True when something is queued (playing or still pre-buffering).
    bool pending() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return !q_.empty();
    }
    size_t queuedFrames() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }
    bool playing() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return playing_;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        q_.clear();
        playing_ = false;
    }
    const Config& config() const { return cfg_; }

    static Clock defaultClock() {
        return [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    }

private:
    Config cfg_;
    Clock clock_;
    mutable std::mutex mtx_;
    std::deque<StereoFrame> q_;
    bool playing_ = false;
    double firstQueuedAt_ = 0.0;
};

}  // namespace sdrstar
