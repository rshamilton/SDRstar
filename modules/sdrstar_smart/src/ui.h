// SPDX-License-Identifier: GPL-3.0-or-later
//
// SmartUi: everything drawn by SDRstar: signal labels on the waterfall, the card next to the cursor, and the
// side-panel menu.  It only reads from SmartEngine and calls doAction().
#pragma once

#include <vector>

#include <gui/gui.h>
#include <gui/widgets/waterfall.h>
#include <imgui.h>

#include "engine.h"

class SmartUi {
public:
    explicit SmartUi(SmartEngine& e) : eng_(e) {}
    void attach();
    void detach();
    void drawMenu();

private:
    struct Hit {
        ImVec2 min, max;
        CardAction action;
        bool enabled;
    };
    static void onRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx);
    static void onInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx);
    void drawLabels(const ImGui::WaterFall::FFTRedrawArgs& a);
    void drawCard(const ImGui::WaterFall::FFTRedrawArgs& a);

    SmartEngine& eng_;
    EventHandler<ImGui::WaterFall::FFTRedrawArgs> redrawHandler_;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler_;
    bool attached_ = false;

    // geometry of the card from the last frame (for hit testing)
    bool cardShown_ = false;
    ImVec2 cardMin_, cardMax_;
    std::vector<Hit> hits_;
    int hoverRow_ = -1;
};
