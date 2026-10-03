#ifndef slic3r_GUI_ImGuiTouchStep_hpp_
#define slic3r_GUI_ImGuiTouchStep_hpp_

#include <algorithm>
#include <cmath>

#include <imgui/imgui.h>

namespace Slic3r { namespace GUI {

struct TouchStep
{
    double fine;
    double coarse;
    double min;
    double max;
    // Step onto multiples of the step, so 97 + 5 gives 100 rather than 102.
    bool   snap { false };
};

constexpr double TouchStepHoldDelay  = 0.5;
constexpr double TouchStepHoldRepeat = 0.4;

inline double touch_step_value(double value, double step, bool plus, bool snap)
{
    if (!snap)
        return plus ? value + step : value - step;
    // Half of the last shown digit of "%.2f", so a value displayed on the grid counts as on it.
    constexpr double on_grid = 0.005;
    return plus ? (std::floor((value + on_grid) / step) + 1.) * step : (std::ceil((value - on_grid) / step) - 1.) * step;
}

// A finger-sized "-" or "+" button. A tap steps by step.fine when the finger lifts. Holding it steps
// by step.coarse after TouchStepHoldDelay and again every TouchStepHoldRepeat, and the lift then adds
// nothing. now is in seconds; ImGui's own clock only advances per rendered frame here.
// Returns true when value changed; *held is set while the button is held down.
inline bool touch_step_button(const char* str_id, bool plus, double& value, const TouchStep& step, float size, double now, bool* held = nullptr)
{
    struct Hold
    {
        ImGuiID id { 0 };
        double  start { 0. };
        int     fired { 0 };
    };
    static Hold hold;

    ImGui::PushID(str_id);
    const ImGuiID id      = ImGui::GetID(plus ? "+" : "-");
    const bool    pressed = ImGui::Button(plus ? "+" : "-", ImVec2(size, size));
    ImGui::PopID();

    if (ImGui::IsItemActivated())
        hold = Hold { id, now, 0 };

    bool fine   = false;
    int  coarse = 0;
    if (ImGui::IsItemActive() && hold.id == id) {
        if (held != nullptr)
            *held = true;
        const double t   = now - hold.start;
        const int    due = t < TouchStepHoldDelay ? 0 : 1 + int((t - TouchStepHoldDelay) / TouchStepHoldRepeat);
        coarse           = due - hold.fired;
        hold.fired       = due;
    } else if (hold.id == id) {
        fine    = pressed && hold.fired == 0;
        hold.id = 0;
    } else
        fine = pressed;

    double v = value;
    if (fine)
        v = touch_step_value(v, step.fine, plus, step.snap);
    for (int i = 0; i < coarse; ++i)
        v = touch_step_value(v, step.coarse, plus, step.snap);
    v = std::clamp(v, step.min, step.max);
    if (v == value)
        return false;
    value = v;
    return true;
}

inline bool touch_step_button(const char* str_id, bool plus, float& value, const TouchStep& step, float size, double now, bool* held = nullptr)
{
    double v       = value;
    const bool ret = touch_step_button(str_id, plus, v, step, size, now, held);
    value          = float(v);
    return ret;
}

}} // namespace Slic3r::GUI

#endif
