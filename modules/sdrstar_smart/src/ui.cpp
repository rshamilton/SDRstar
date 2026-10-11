// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <gui/style.h>
#include <imgui_internal.h>
#include <sdrstar/window_planner.h>

namespace {

ImU32 toneColor(CardTone t) {
    switch (t) {
    case CardTone::Working: return IM_COL32(90, 170, 255, 255);
    case CardTone::Good: return IM_COL32(90, 220, 130, 255);
    case CardTone::Attention: return IM_COL32(255, 190, 70, 255);
    case CardTone::Problem: return IM_COL32(255, 90, 90, 255);
    default: return IM_COL32(170, 180, 195, 255);
    }
}

ImU32 labelColor(const sdrstar::DetectedSignal& s) {
    if (!s.identified.empty()) { return IM_COL32(120, 255, 160, 255); }
    if (s.label == "control channel?") { return IM_COL32(255, 200, 90, 255); }
    if (s.kind == sdrstar::SignalKind::WideFM) { return IM_COL32(130, 190, 255, 255); }
    if (s.kind == sdrstar::SignalKind::Wideband) { return IM_COL32(190, 150, 255, 255); }
    return IM_COL32(215, 220, 230, 235);
}

}  // namespace

void SmartUi::attach() {
    if (attached_) { return; }
    redrawHandler_.ctx = this;
    redrawHandler_.handler = onRedraw;
    inputHandler_.ctx = this;
    inputHandler_.handler = onInput;
    gui::waterfall.onFFTRedraw.bindHandler(&redrawHandler_);
    gui::waterfall.onInputProcess.bindHandler(&inputHandler_);
    attached_ = true;
}

void SmartUi::detach() {
    if (!attached_) { return; }
    gui::waterfall.onFFTRedraw.unbindHandler(&redrawHandler_);
    gui::waterfall.onInputProcess.unbindHandler(&inputHandler_);
    attached_ = false;
}

void SmartUi::onRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
    SmartUi* u = (SmartUi*)ctx;
    u->eng_.tick();  // once per frame, on the GUI thread
    if (u->eng_.settings().showLabels) { u->drawLabels(args); }
    u->drawCard(args);
}

// ------------------------------------------------------------------------------------------------------------
// labels
// ------------------------------------------------------------------------------------------------------------
void SmartUi::drawLabels(const ImGui::WaterFall::FFTRedrawArgs& a) {
    ImDrawList* dl = a.window->DrawList;
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.82f;
    const float pad = 3.0f * style::uiScale;
    const float rowH = fs + 2 * pad + 2.0f;

    const SmartEngine::SignalList list = eng_.signals();
    labelOrder_.clear();
    for (const auto& s : *list) { labelOrder_.push_back(&s); }
    // strongest first so important labels win collisions
    std::sort(labelOrder_.begin(), labelOrder_.end(), [](const sdrstar::DetectedSignal* x, const sdrstar::DetectedSignal* y) { return x->snrDb > y->snrDb; });

    struct Box { float x0, x1; };
    std::vector<Box> rows[4];
    const float top = a.min.y + 2.0f;
    int drawn = 0;
    for (const sdrstar::DetectedSignal* sp : labelOrder_) {
        const sdrstar::DetectedSignal& s = *sp;
        if (s.freqHz < a.lowFreq || s.freqHz > a.highFreq) { continue; }
        if (!s.present && s.identified.empty()) { continue; }
        const float cx = a.min.x + (float)((s.freqHz - a.lowFreq) * a.freqToPixelRatio);
        std::string text = s.label;
        if (s.activity < 0.3 && s.identified.empty()) { text += " (intermittent)"; }
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
        const float w = ts.x + 2 * pad;
        float x0 = std::clamp(cx - w / 2, a.min.x + 2.0f, a.max.x - w - 2.0f);
        int row = -1;
        for (int r = 0; r < 4 && row < 0; ++r) {
            bool clash = false;
            for (const auto& b : rows[r]) {
                if (x0 < b.x1 + 3.0f && x0 + w > b.x0 - 3.0f) { clash = true; break; }
            }
            if (!clash) { row = r; }
        }
        if (row < 0) { continue; }
        rows[row].push_back({x0, x0 + w});
        const float y0 = top + row * rowH;
        const ImU32 col = labelColor(s);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + ts.y + 2 * pad), IM_COL32(8, 10, 16, 190), 3.0f * style::uiScale);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + ts.y + 2 * pad), (col & 0x00FFFFFF) | 0x80000000, 3.0f * style::uiScale);
        dl->AddText(font, fs, ImVec2(x0 + pad, y0 + pad), col, text.c_str());
        // little pointer down to the signal
        const float py = y0 + ts.y + 2 * pad;
        dl->AddTriangleFilled(ImVec2(cx - 3, py), ImVec2(cx + 3, py), ImVec2(cx, py + 5), col);
        if (++drawn > 40) { break; }
    }
}

// ------------------------------------------------------------------------------------------------------------
// the card
// ------------------------------------------------------------------------------------------------------------
void SmartUi::drawCard(const ImGui::WaterFall::FFTRedrawArgs& a) {
    cardShown_ = false;
    hits_.clear();
    CardModel m = eng_.card();
    if (!m.visible) { return; }

    if (m.compact) {  // small background-activity chip in the bottom-left corner of the spectrum
        ImDrawList* cdl = a.window->DrawList;
        ImFont* cf = ImGui::GetFont();
        const float cs = ImGui::GetFontSize() * 0.85f;
        const float u0 = style::uiScale;
        ImVec2 ts = cf->CalcTextSizeA(cs, FLT_MAX, 0.0f, m.title.c_str());
        const float cw = ts.x + 20.0f * u0, ch = ts.y + 12.0f * u0;
        ImVec2 cmin(a.min.x + 8.0f * u0, a.max.y - ch - 8.0f * u0), cmax(cmin.x + cw, cmin.y + ch);
        cdl->AddRectFilled(cmin, cmax, IM_COL32(14, 17, 26, 215), 5.0f * u0);
        cdl->AddRect(cmin, cmax, IM_COL32(90, 170, 255, 120), 5.0f * u0);
        cdl->AddText(cf, cs, ImVec2(cmin.x + 10.0f * u0, cmin.y + 6.0f * u0), IM_COL32(190, 210, 235, 255), m.title.c_str());
        if (m.progress >= 0) {
            cdl->AddRectFilled(ImVec2(cmin.x + 4.0f * u0, cmax.y - 3.0f * u0), ImVec2(cmin.x + 4.0f * u0 + (cw - 8.0f * u0) * m.progress, cmax.y - 1.0f * u0), IM_COL32(90, 170, 255, 255));
        }
        return;
    }

    ImDrawList* dl = a.window->DrawList;
    ImFont* font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    const float u = style::uiScale;
    const float W = 350.0f * u;
    const float pad = 10.0f * u;
    const float innerW = W - 2 * pad - 6.0f * u;

    // --- measure ---
    const float titleSize = base * 1.08f;
    const float smallSize = base * 0.88f;
    ImVec2 tsz = font->CalcTextSizeA(titleSize, FLT_MAX, innerW - 22.0f * u, m.title.c_str());
    float h = pad + tsz.y;
    ImVec2 ssz(0, 0);
    if (!m.subtitle.empty()) {
        ssz = font->CalcTextSizeA(smallSize, FLT_MAX, innerW, m.subtitle.c_str());
        h += 3.0f * u + ssz.y;
    }
    if (m.progress >= 0) { h += 8.0f * u + 5.0f * u; }
    std::vector<ImVec2> lineSizes;
    for (const auto& l : m.lines) {
        ImVec2 sz = font->CalcTextSizeA(smallSize, FLT_MAX, innerW, l.c_str());
        lineSizes.push_back(sz);
        h += 5.0f * u + sz.y;
    }
    // tips: grey explanatory text with a small "x" at the right that hides it for good
    const float tipX = 14.0f * u;
    const float tipW = innerW - tipX - 6.0f * u;
    std::vector<ImVec2> tipSizes;
    for (const auto& t : m.tips) {
        ImVec2 sz = font->CalcTextSizeA(smallSize, FLT_MAX, tipW, t.text.c_str());
        tipSizes.push_back(sz);
        h += 5.0f * u + std::max(sz.y, tipX);
    }
    ImVec2 wsz(0, 0);
    if (!m.warning.empty()) {
        wsz = font->CalcTextSizeA(smallSize, FLT_MAX, innerW, m.warning.c_str());
        h += 6.0f * u + wsz.y;
    }
    // buttons: flow layout (the dismiss "x" is drawn separately in the corner)
    const float btnH = base * 1.7f;
    std::vector<std::pair<const CardButton*, float>> btns;
    int rowsOfButtons = 0;
    {
        float x = 0;
        bool any = false;
        for (const auto& b : m.buttons) {
            if (b.action == ACT_DISMISS) { continue; }
            float bw = font->CalcTextSizeA(smallSize, FLT_MAX, 0, b.label.c_str()).x + 2 * 10.0f * u;
            if (x > 0 && x + bw > innerW) {
                rowsOfButtons++;
                x = 0;
            }
            x += bw + 6.0f * u;
            any = true;
        }
        if (any) { rowsOfButtons++; }
    }
    if (rowsOfButtons) { h += 8.0f * u + rowsOfButtons * (btnH + 5.0f * u) - 5.0f * u; }
    h += pad;

    // --- place near the cursor / job frequency ---
    const float fx = a.min.x + (float)((m.freqHz - a.lowFreq) * a.freqToPixelRatio);
    float x0 = fx + 95.0f * u;                 // clear of SDR++'s own VFO name label
    if (x0 + W > a.max.x - 6.0f) { x0 = fx - W - 95.0f * u; }
    x0 = std::clamp(x0, a.min.x + 6.0f, std::max(a.min.x + 6.0f, a.max.x - W - 6.0f));
    float y0 = a.min.y + 5.0f * u + 4 * (base * 0.82f + 8.0f * u);  // below the label rows
    const float maxY = a.max.y - h - 6.0f;
    if (y0 > maxY) { y0 = std::max(a.min.y + 4.0f, maxY); }
    cardMin_ = ImVec2(x0, y0);
    cardMax_ = ImVec2(x0 + W, y0 + h);
    cardShown_ = true;

    const ImU32 accent = toneColor(m.tone);
    dl->AddRectFilled(cardMin_, cardMax_, IM_COL32(14, 17, 26, 238), 7.0f * u);
    dl->AddRect(cardMin_, cardMax_, (accent & 0x00FFFFFF) | 0xA0000000, 7.0f * u, 0, 1.5f * u);
    dl->AddRectFilled(cardMin_, ImVec2(x0 + 5.0f * u, y0 + h), accent, 7.0f * u, ImDrawFlags_RoundCornersLeft);

    float y = y0 + pad;
    const float tx = x0 + pad + 6.0f * u;
    dl->AddText(font, titleSize, ImVec2(tx, y), IM_COL32(245, 247, 252, 255), m.title.c_str(), NULL, innerW - 22.0f * u);
    y += tsz.y;
    if (!m.subtitle.empty()) {
        y += 3.0f * u;
        dl->AddText(font, smallSize, ImVec2(tx, y), IM_COL32(170, 182, 200, 255), m.subtitle.c_str(), NULL, innerW);
        y += ssz.y;
    }
    if (m.progress >= 0) {
        y += 8.0f * u;
        dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + innerW, y + 5.0f * u), IM_COL32(45, 52, 68, 255), 2.5f * u);
        dl->AddRectFilled(ImVec2(tx, y), ImVec2(tx + innerW * std::clamp(m.progress, 0.0f, 1.0f), y + 5.0f * u), accent, 2.5f * u);
        y += 5.0f * u;
    }
    for (size_t i = 0; i < m.lines.size(); ++i) {
        y += 5.0f * u;
        dl->AddText(font, smallSize, ImVec2(tx, y), IM_COL32(140, 152, 170, 255), m.lines[i].c_str(), NULL, innerW);
        y += lineSizes[i].y;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    for (size_t i = 0; i < m.tips.size(); ++i) {
        y += 5.0f * u;
        dl->AddText(font, smallSize, ImVec2(tx, y), IM_COL32(140, 152, 170, 255), m.tips[i].text.c_str(), NULL, tipW);
        ImVec2 xmin(tx + innerW - tipX, y), xmax(xmin.x + tipX, xmin.y + tipX);
        const bool xhover = mouse.x >= xmin.x && mouse.x <= xmax.x && mouse.y >= xmin.y && mouse.y <= xmax.y;
        dl->AddRectFilled(xmin, xmax, xhover ? IM_COL32(90, 60, 66, 255) : IM_COL32(32, 38, 52, 255), 3.0f * u);
        ImVec2 xl = font->CalcTextSizeA(smallSize * 0.9f, FLT_MAX, 0, "x");
        dl->AddText(font, smallSize * 0.9f, ImVec2(xmin.x + (tipX - xl.x) / 2, xmin.y + (tipX - xl.y) / 2), xhover ? IM_COL32(240, 225, 225, 255) : IM_COL32(150, 160, 178, 255), "x");
        hits_.push_back({xmin, xmax, ACT_HIDE_TIP, true, m.tips[i].key});
        y += std::max(tipSizes[i].y, tipX);
    }
    if (!m.warning.empty()) {
        y += 6.0f * u;
        dl->AddText(font, smallSize, ImVec2(tx, y), IM_COL32(255, 120, 120, 255), m.warning.c_str(), NULL, innerW);
        y += wsz.y;
    }

    // --- buttons ---
    if (rowsOfButtons) {
        y += 8.0f * u;
        float bx = 0;
        for (const auto& b : m.buttons) {
            if (b.action == ACT_DISMISS) { continue; }
            ImVec2 lt = font->CalcTextSizeA(smallSize, FLT_MAX, 0, b.label.c_str());
            float bw = lt.x + 2 * 10.0f * u;
            if (bx > 0 && bx + bw > innerW) {
                bx = 0;
                y += btnH + 5.0f * u;
            }
            ImVec2 bmin(tx + bx, y), bmax(tx + bx + bw, y + btnH);
            const bool hover = b.enabled && mouse.x >= bmin.x && mouse.x <= bmax.x && mouse.y >= bmin.y && mouse.y <= bmax.y;
            ImU32 fill = b.primary ? (hover ? IM_COL32(70, 130, 235, 255) : IM_COL32(50, 105, 210, 255)) : (hover ? IM_COL32(70, 78, 98, 255) : IM_COL32(48, 55, 72, 255));
            if (!b.enabled) { fill = IM_COL32(40, 44, 54, 255); }
            dl->AddRectFilled(bmin, bmax, fill, 4.0f * u);
            dl->AddText(font, smallSize, ImVec2(bmin.x + 10.0f * u, bmin.y + (btnH - lt.y) / 2), b.enabled ? IM_COL32(240, 244, 250, 255) : IM_COL32(120, 128, 142, 255), b.label.c_str());
            hits_.push_back({bmin, bmax, b.action, b.enabled, std::string()});
            bx += bw + 6.0f * u;
        }
    }
    // dismiss "x" in the top-right corner (only for cursor cards, never for an active job)
    for (const auto& b : m.buttons) {
        if (b.action != ACT_DISMISS) { continue; }
        const float s = 18.0f * u;
        ImVec2 bmin(x0 + W - s - 6.0f * u, y0 + 6.0f * u), bmax(bmin.x + s, bmin.y + s);
        const bool hover = mouse.x >= bmin.x && mouse.x <= bmax.x && mouse.y >= bmin.y && mouse.y <= bmax.y;
        dl->AddRectFilled(bmin, bmax, hover ? IM_COL32(90, 60, 66, 255) : IM_COL32(36, 42, 56, 255), 4.0f * u);
        ImVec2 lt = font->CalcTextSizeA(smallSize, FLT_MAX, 0, "x");
        dl->AddText(font, smallSize, ImVec2(bmin.x + (s - lt.x) / 2, bmin.y + (s - lt.y) / 2), IM_COL32(220, 225, 235, 255), "x");
        hits_.push_back({bmin, bmax, ACT_DISMISS, true, std::string()});
    }
}

// ------------------------------------------------------------------------------------------------------------
// input: clicks on the card must not move the VFO or pan the waterfall
// ------------------------------------------------------------------------------------------------------------
void SmartUi::onInput(ImGui::WaterFall::InputHandlerArgs, void* ctx) {
    SmartUi* u = (SmartUi*)ctx;
    if (!u->cardShown_) { return; }
    const ImVec2 mp = ImGui::GetMousePos();
    const bool over = mp.x >= u->cardMin_.x && mp.x <= u->cardMax_.x && mp.y >= u->cardMin_.y && mp.y <= u->cardMax_.y;
    if (!over) { return; }
    gui::waterfall.inputHandled = true;
    for (const auto& h : u->hits_) {
        if (h.enabled && mp.x >= h.min.x && mp.x <= h.max.x && mp.y >= h.min.y && mp.y <= h.max.y) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (h.action == ACT_HIDE_TIP) { ImGui::SetTooltip("Hide this tip for good.\nTurn tips back on in the SDRstar panel."); }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { u->eng_.doAction(h.action, h.arg); }
            return;
        }
    }
}

// ------------------------------------------------------------------------------------------------------------
// side panel
// ------------------------------------------------------------------------------------------------------------
void SmartUi::drawMenu() {
    EngineSettings& s = eng_.settings();
    const float w = ImGui::GetContentRegionAvail().x;
    bool changed = false;

    ImGui::TextWrapped("%s", eng_.sourceSummary().c_str());
    if (!eng_.decoderAvailable()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
        ImGui::TextWrapped("Digital decoder (dsd-neo) not found. Signals are still detected and labelled, but cannot be identified or followed.");
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    changed |= ImGui::Checkbox("Label signals on the waterfall##sdrstar", &s.showLabels);
    changed |= ImGui::Checkbox("Quietly identify steady digital signals##sdrstar", &s.autoIdentify);
    changed |= ImGui::Checkbox("Auto-decode identified signals##sdrstar", &s.autoDecode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON: rest the cursor on a signal that has been identified (about half a second), or press Identify and let\n"
                          "it find a protocol, and decoding starts by itself (a trunked system's control channel is followed).\n"
                          "It never interrupts a decoder that is already running, and after you press Stop it waits until you\n"
                          "move the cursor to somewhere new.");
    }
    ImGui::TextUnformatted("Sensitivity: signal must rise this many dB above noise");
    ImGui::SetNextItemWidth(w);
    float sens = (float)s.sensitivityDb;
    if (ImGui::SliderFloat("##sdrstar_sens", &sens, 4.0f, 20.0f, "%.0f dB")) {
        s.sensitivityDb = sens;
        changed = true;
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How far above the noise a signal must rise to be labelled.\nLower finds weaker signals, higher gives fewer false labels."); }

    {
        bool tipsOn = s.tips.enabled();
        if (ImGui::Checkbox("Show explanatory tips on cards##sdrstar", &tipsOn)) {
            s.tips.setEnabled(tipsOn);
            changed = true;
        }
        if (s.tips.hiddenCount() > 0) {
            char lbl[96];
            std::snprintf(lbl, sizeof lbl, "Bring back %d hidden tip%s##sdrstar_tips", (int)s.tips.hiddenCount(), s.tips.hiddenCount() == 1 ? "" : "s");
            if (ImGui::Button(lbl, ImVec2(w, 0))) { eng_.restoreTips(); }
        }
    }

    ImGui::Spacing();
    const bool busy = eng_.jobKind() != JobKind::None;
    // Decide once: pressing the button changes the job state, and begin/endDisabled must stay paired.
    const bool identifyBlocked = !eng_.cursorValid() || eng_.jobKind() == JobKind::Probing || !eng_.decoderAvailable();
    if (identifyBlocked) { style::beginDisabled(); }
    const bool identifyPressed = ImGui::Button("Identify signal under the cursor##sdrstar", ImVec2(w, 0));
    if (identifyBlocked) { style::endDisabled(); }
    if (identifyPressed) { eng_.doAction(ACT_IDENTIFY); }

    ImGui::Spacing();
    ImGui::Text("Status");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.82f, 0.95f, 1.0f));
    ImGui::TextWrapped("%s", eng_.decoderStatusLine().c_str());
    ImGui::PopStyleColor();
    if (busy) {
        if (ImGui::Button("Stop##sdrstar_stop", ImVec2(w, 0))) { eng_.doAction(ACT_STOP); }
    }

    ImGui::Separator();
    ImGui::Text("Text decoders");
    {
        static int pick = 0;
        static const char* ids[] = {"pager", "aprs", "eas", "ism", "adsb"};
        static const char* names[] = {"Pager messages (POCSAG / FLEX)", "APRS packets", "Weather alerts (EAS)", "ISM sensors (433 / 868 / 915 MHz)", "Aircraft (ADS-B, 1090 MHz)"};
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##sdrstar_helper", names[pick])) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(names[i], pick == i)) { pick = i; }
            }
            ImGui::EndCombo();
        }
        const bool fixedFreq = std::string(ids[pick]) == "adsb";   // always 1090 MHz: no cursor needed
        const bool can = (fixedFreq || eng_.cursorValid()) && eng_.helperAvailable(ids[pick]);
        if (!can) { style::beginDisabled(); }
        if (ImGui::Button(fixedFreq ? "Listen on 1090 MHz##sdrstar_helper_go" : "Decode at the cursor##sdrstar_helper_go", ImVec2(w, 0))) {
            eng_.startHelper(ids[pick], eng_.cursorFreq());
        }
        if (!can) { style::endDisabled(); }
        if (!eng_.helperAvailable(ids[pick])) { ImGui::TextDisabled("This decoder was not found in the app bundle."); }
    }
    if (eng_.jobKind() == JobKind::Helper) {
        auto ls = eng_.helperLines(80);
        if (ImGui::TreeNodeEx("Messages##sdrstar_msgs", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ls.empty()) { ImGui::TextDisabled("Nothing received yet."); }
            for (auto it = ls.rbegin(); it != ls.rend(); ++it) {
                if (it->repeat > 1) {
                    ImGui::TextWrapped("[%5.0fs] %s  (x%d)", it->tSec, it->text.c_str(), it->repeat);
                } else {
                    ImGui::TextWrapped("[%5.0fs] %s", it->tSec, it->text.c_str());
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::Separator();
    ImGui::Text("Trunk following");
    if (ImGui::Checkbox("Hop to out-of-range calls##sdrstar", &s.allowHop)) {
        eng_.settingsChanged();
        changed = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("One RTL-SDR can only see ~2 MHz at a time. If a call is further from the control channel than that,\n"
                          "ON: leave the control channel for the call (new call announcements are missed meanwhile).\n"
                          "OFF: stay on the control channel and skip such calls (the reason is shown).");
    }
    static char mapBuf[512];
    static char grpBuf[512];
    static bool init = false;
    if (!init) {
        std::strncpy(mapBuf, s.channelMapPath.c_str(), sizeof mapBuf - 1);
        std::strncpy(grpBuf, s.groupListPath.c_str(), sizeof grpBuf - 1);
        init = true;
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##sdrstar_map", "Channel list CSV (DMR / NXDN / EDACS)", mapBuf, sizeof mapBuf)) {
        s.channelMapPath = mapBuf;
        changed = true;
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##sdrstar_grp", "Talkgroup list CSV (optional)", grpBuf, sizeof grpBuf)) {
        s.groupListPath = grpBuf;
        changed = true;
    }
    ImGui::TextDisabled("P25 systems need no list: the plan is learned from the control channel.");

    if (eng_.jobKind() == JobKind::Following) {
        sdrstar::FollowStatus st = eng_.followStatus();
        ImGui::Separator();
        ImGui::TextWrapped("%s", st.headline.c_str());
        if (!st.system.empty()) { ImGui::TextDisabled("%s", st.system.c_str()); }
        if (!st.problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", st.problem.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::TreeNode("Activity log##sdrstar")) {
            for (auto it = st.log.rbegin(); it != st.log.rend(); ++it) {
                ImVec4 col = it->level == sdrstar::FollowEvent::Problem ? ImVec4(1.0f, 0.5f, 0.45f, 1.0f)
                             : it->level == sdrstar::FollowEvent::Warn ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f)
                                                                      : ImVec4(0.7f, 0.75f, 0.85f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("[%5.1fs] %s", it->tSec, it->text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::TreePop();
        }
    }
    if (changed) { eng_.settingsChanged(); }
}
