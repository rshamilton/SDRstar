// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoiceOut: plays decoded digital voice (8 kHz s16 from DSD-neo, mono or interleaved stereo) through SDR++'s normal audio
// system as its own stream ("SDRstar Voice"), so the user gets the usual per-stream volume and output-device selection in
// the Sinks menu.
//
// Why it is built the way it is (two real problems on macOS):
//
//  * SDR++'s audio sinks read their input INSIDE the audio-device callback and wait there until data arrives.  A second
//    stream on the same output device that has nothing to play therefore blocks the device's single I/O thread, and the
//    ordinary "Radio" stream sharing that thread glitches.  So (1) this stream is only open while a decode or follow job
//    runs (begin() / end()), and (2) while it is open it is fed CONTINUOUSLY: silence between calls, so its callback
//    always finds data and never waits.
//
//  * Decoded voice arrives in bursts (DMR: 60 ms at a time).  Played as it arrives, every gap under-runs the device,
//    which repeats stale audio (an echo).  So voice goes through a small jitter buffer (it starts playing once ~80 ms
//    is queued) and is paced out in steady 20 ms pieces.
//
// push() never blocks the caller: data goes through a bounded ring and a feeder thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <dsp/multirate/rational_resampler.h>
#include <dsp/stream.h>
#include <signal_path/sink.h>
#include <utils/event.h>

class VoiceOut {
public:
    VoiceOut() = default;
    ~VoiceOut() { shutdown(); }

    // Register the stream with SDR++ (it shows up in the Sinks menu).  Does not open the audio device.
    void init(const std::string& streamName);
    void shutdown();

    // GUI thread.  begin(): open the output device and start feeding it.  end(): stop feeding and close the device.
    void begin();
    void end();
    bool active() const { return active_.load(); }

    // Thread-safe, non-blocking.  `interleaved` holds frames*channels s16 samples (channels = 1 or 2) at 8 kHz.
    void push(const int16_t* interleaved, size_t frames, int channels, int rate);
    // Drop anything queued (e.g. when a call is abandoned).
    void clear();

    long long framesPlayed() const { return played_.load(); }
    bool initialised() const { return inited_; }

    // Playout tuning (frames at 8 kHz); public so the unit of work is documented in one place.
    static constexpr size_t kChunkFrames = 160;     // 20 ms: what is handed to the audio chain at a time
    static constexpr size_t kStartFrames = 640;     // 80 ms of voice queued before a call starts playing
    static constexpr size_t kMaxFrames = 3200;      // 400 ms: older voice is dropped so latency cannot build up

private:
    void feederLoop();
    static void onSampleRate(float sr, void* ctx);

    std::string name_;
    bool inited_ = false;
    dsp::stream<dsp::stereo_t> in_;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp_;
    SinkManager::Stream stream_;
    EventHandler<float> srHandler_;
    double inRate_ = 8000.0;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<dsp::stereo_t> ring_;
    std::thread feeder_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};
    bool feederBusy_ = false;           // guarded by mtx_: the feeder is between "got work" and "waiting again"
    std::atomic<long long> played_{0};
};
