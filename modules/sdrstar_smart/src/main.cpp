// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDRstar smart-signal module for SDR++: signal detection and labelling on the waterfall, digital identification,
// decoding and trunk following.  All logic lives in engine.cpp and the pure-C++ sdrstar_core library.
#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

#include "engine.h"
#include "ui.h"

SDRPP_MOD_INFO{
    /* Name:            */ "sdrstar_smart",
    /* Description:     */ "SDRstar: automatic signal detection, digital decoding and trunk following",
    /* Author:          */ "SDRstar",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

class SdrStarModule : public ModuleManager::Instance {
public:
    explicit SdrStarModule(std::string name) : name_(std::move(name)), ui_(engine_) {
        gui::menu.registerEntry(name_, menuHandler, this, this);
        enable();
    }

    ~SdrStarModule() override {
        disable();
        gui::menu.removeEntry(name_);
    }

    void postInit() override {}

    void enable() override {
        if (enabled_) { return; }
        engine_.start(core::args["root"].s());
        ui_.attach();
        enabled_ = true;
    }

    void disable() override {
        if (!enabled_) { return; }
        ui_.detach();
        engine_.stop();
        enabled_ = false;
    }

    bool isEnabled() override { return enabled_; }

private:
    static void menuHandler(void* ctx) {
        SdrStarModule* m = (SdrStarModule*)ctx;
        if (!m->enabled_) { style::beginDisabled(); }
        m->ui_.drawMenu();
        if (!m->enabled_) { style::endDisabled(); }
    }

    std::string name_;
    bool enabled_ = false;
    SmartEngine engine_;
    SmartUi ui_;
};

MOD_EXPORT void _INIT_() {}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new SdrStarModule(name); }

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) { delete (SdrStarModule*)instance; }

MOD_EXPORT void _END_() {}
