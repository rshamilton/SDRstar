// SPDX-License-Identifier: GPL-3.0-or-later
//
// SdrppRadioHost: the RadioHost implementation on top of SDR++ (hidden channels through iqFrontEnd.addVFO,
// hardware retunes through tuner::iqTuning).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <dsp/sink/handler_sink.h>
#include <dsp/channel/rx_vfo.h>
#include <sdrstar/radio_host.h>

class SdrppRadioHost : public sdrstar::RadioHost {
public:
    SdrppRadioHost() = default;
    ~SdrppRadioHost() override;

    sdrstar::ReceiverCaps caps() const override;
    double centerHz() const override;
    bool setCenter(double hz, std::string& error) override;
    double channelRateHz() const override { return 48000.0; }
    int openChannel(double absFreqHz, double bwHz, IqSinkFn sink, std::string& error) override;
    bool moveChannel(int channel, double absFreqHz) override;
    void closeChannel(int channel) override;

    // Name of the selected SDR++ source ("RTL-SDR", "File", ...), and whether the hardware can be retuned.
    std::string sourceName() const;
    bool retunable() const;
    // Close every hidden channel (module shutdown).
    void closeAll();

private:
    struct Chan {
        std::string name;
        dsp::channel::RxVFO* vfo = nullptr;
        dsp::sink::Handler<dsp::complex_t> handler;
        IqSinkFn sink;
        double absHz = 0;
        double bwHz = 0;
    };
    static void onBlock(dsp::complex_t* data, int count, void* ctx);
    bool fitsLocked(double absHz, double bwHz) const;
    void applyOffsetsLocked();

    mutable std::mutex mtx_;
    std::map<int, std::unique_ptr<Chan>> chans_;
    int nextId_ = 1;
};
