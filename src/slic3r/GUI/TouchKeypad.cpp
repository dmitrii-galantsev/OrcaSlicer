#include "TouchKeypad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

#include <wx/combo.h>
#include <wx/dataview.h>
#include <wx/display.h>
#include <wx/grid.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valnum.h>
#include <wx/valtext.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/TempInput.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

std::optional<TouchNumber> parse_touch_number(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return std::nullopt;
    std::string s = text.substr(first, text.find_last_not_of(" \t") - first + 1);

    std::string suffix;
    if (s.back() == '%')
        suffix = "%";
    else if (s.size() >= 2 && s.compare(s.size() - 2, 2, "mm") == 0)
        suffix = "mm";
    s.resize(s.size() - suffix.size());
    const size_t number_end = s.find_last_not_of(" \t");
    if (number_end == std::string::npos)
        return std::nullopt;
    suffix = s.substr(number_end + 1) + suffix;
    s.resize(number_end + 1);

    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    int    int_digits = 0, decimals = 0;
    bool   dot = false;
    for (; i < s.size(); ++i) {
        if (s[i] == '.' && !dot)
            dot = true;
        else if (s[i] >= '0' && s[i] <= '9')
            ++(dot ? decimals : int_digits);
        else
            return std::nullopt;
    }
    if (int_digits + decimals == 0)
        return std::nullopt;

    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double value = 0.;
    is >> value;
    return TouchNumber{s, value, decimals, suffix};
}

TouchSteps touch_steps(const TouchNumber& number)
{
    if (number.decimals == 0)
        return {1., 10., 0};
    const int decimals = std::abs(number.value) < 1. ? std::max(number.decimals, 2) : number.decimals;
    const double fine = std::pow(10., -decimals);
    return {fine, fine * 10., decimals};
}

std::string format_touch_number(double value, int min_decimals, int max_decimals)
{
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(std::max(max_decimals, 0)) << value;
    std::string s = os.str();
    if (const size_t dot = s.find('.'); dot != std::string::npos) {
        const size_t keep = dot + 1 + std::max(min_decimals, 0);
        while (s.size() > keep && s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s[0] == '-' && s.find_first_of("123456789") == std::string::npos)
        s.erase(0, 1);
    return s;
}

std::string step_touch_number(const std::string& number_text, double delta, int min_decimals, const TouchSteps& steps)
{
    const auto number   = parse_touch_number(number_text);
    const int  decimals = std::max(steps.decimals, number ? number->decimals : 0);
    return format_touch_number((number ? number->value : 0.) + delta, std::min(min_decimals, decimals), decimals);
}

namespace {

bool s_keypad_open = false;

const wxString MINUS     = wxString(L"\u2212");
const wxString BACKSPACE = wxString(L"\u232B");
const wxString SHIFT     = wxString(L"\u21E7");

bool has_numeric_validator(wxWindow* window)
{
    wxValidator* validator = window->GetValidator();
    if (dynamic_cast<wxNumValidatorBase*>(validator) != nullptr)
        return true;
    auto* text_validator = dynamic_cast<wxTextValidator*>(validator);
    return text_validator != nullptr && (text_validator->HasFlag(wxFILTER_DIGITS) || text_validator->HasFlag(wxFILTER_NUMERIC));
}

// In-place cell editors end editing as soon as they lose focus to the keypad, so the value
// would land in a destroyed control.
bool is_cell_editor(wxWindow* window)
{
    for (wxWindow* w = window->GetParent(); w != nullptr && !w->IsTopLevel(); w = w->GetParent())
        if (dynamic_cast<wxDataViewCtrl*>(w) != nullptr || dynamic_cast<wxGrid*>(w) != nullptr)
            return true;
    return false;
}

bool is_spin(wxWindow* window)
{
    return dynamic_cast<wxSpinCtrl*>(window) != nullptr || dynamic_cast<wxSpinCtrlDouble*>(window) != nullptr;
}

bool accepts_touch_keypad(wxWindow* target)
{
    if (!target->IsEnabled() || !target->IsShownOnScreen() || is_cell_editor(target))
        return false;
    if (is_spin(target))
        return true;
    if (auto* text = dynamic_cast<wxTextCtrl*>(target); text != nullptr && text->IsMultiLine())
        return false;
    auto* entry = dynamic_cast<wxTextEntry*>(target);
    return entry != nullptr && entry->IsEditable();
}

// Orca's composite inputs draw their frame, icon and "mm"/"%" side text on a container around the
// wxTextCtrl, so a tap there must be routed to the inner control.
wxWindow* touch_target(wxWindow* window, const wxPoint& pos)
{
    if (auto* combo = dynamic_cast<::ComboBox*>(window)) {
        wxTextCtrl* text = combo->GetTextCtrl();
        // Right of the text is the drop-down arrow, which keeps opening the list.
        return text != nullptr && text->IsShown() && pos.x <= text->GetRect().GetRight() ? text : nullptr;
    }
    if (auto* input = dynamic_cast<::TextInput*>(window))
        return input->GetTextCtrl();
    if (auto* input = dynamic_cast<::SpinInput*>(window))
        return input->GetTextCtrl();
    if (auto* input = dynamic_cast<::TempInput*>(window))
        return input->GetTextCtrl();
    if (is_spin(window)) {
        // GTK draws the -/+ buttons inside the spin control, each about as wide as it is tall. The
        // press may come from the buttons' own GdkWindow, so use the pointer, not the event position.
        const wxSize  size = window->GetClientSize();
        const wxPoint at   = window->ScreenToClient(wxGetMousePosition());
        return at.x < std::max(size.x - 2 * size.y, size.x / 3) ? window : nullptr;
    }
    if (dynamic_cast<wxTextEntry*>(window) != nullptr && dynamic_cast<wxItemContainer*>(window) == nullptr &&
        dynamic_cast<wxComboCtrl*>(window) == nullptr)
        return window;
    return nullptr;
}

void commit_spin(wxWindow* target, const wxString& text)
{
    const auto number = parse_touch_number(into_u8(text));
    if (!number)
        return;
    if (auto* spin = dynamic_cast<wxSpinCtrl*>(target)) {
        const long value = std::clamp(std::lround(number->value), long(spin->GetMin()), long(spin->GetMax()));
        if (value == spin->GetValue())
            return;
        spin->SetValue(int(value));
        wxSpinEvent evt(wxEVT_SPINCTRL, spin->GetId());
        evt.SetEventObject(spin);
        evt.SetPosition(spin->GetValue());
        evt.SetString(spin->GetTextValue());
        spin->HandleWindowEvent(evt);
    } else if (auto* spin = dynamic_cast<wxSpinCtrlDouble*>(target)) {
        const double value = std::clamp(number->value, spin->GetMin(), spin->GetMax());
        if (value == spin->GetValue())
            return;
        spin->SetValue(value);
        wxSpinDoubleEvent evt(wxEVT_SPINCTRLDOUBLE, spin->GetId());
        evt.SetEventObject(spin);
        evt.SetValue(spin->GetValue());
        evt.SetString(spin->GetTextValue());
        spin->HandleWindowEvent(evt);
    }
}

void place_on_screen(wxDialog* dlg, wxWindow* parent)
{
    dlg->Layout();
    dlg->Fit();
    if (parent != nullptr)
        dlg->CentreOnParent();
    else
        dlg->Centre();
    const int display = wxDisplay::GetFromWindow(parent != nullptr ? parent : dlg);
    const wxRect area = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
    wxRect rect = dlg->GetRect();
    rect.x = std::max(area.x, std::min(rect.x, area.GetRight() - rect.width));
    rect.y = std::max(area.y, std::min(rect.y, area.GetBottom() - rect.height));
    dlg->Move(rect.GetPosition());
}

} // namespace

TouchKeypad::TouchKeypad(wxWindow* parent, const wxString& value, bool password, bool numeric_hint)
    : wxDialog(parent, wxID_ANY, _L("Enter value"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_password(password)
    , m_text(value)
    , m_repeat_timer(this)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Head_20);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_display = new wxStaticText(this, wxID_ANY, " ", wxDefaultPosition, wxSize(FromDIP(560), -1),
                                 wxST_NO_AUTORESIZE | wxALIGN_CENTRE_HORIZONTAL | wxST_ELLIPSIZE_START);
    m_display->SetFont(Label::Head_32);
    m_display->SetMinSize(wxSize(FromDIP(560), m_display->GetCharHeight() + FromDIP(16)));
    sizer->Add(m_display, 0, wxEXPAND | wxALL, FromDIP(12));

    build_number_panel();
    build_text_panel();
    sizer->Add(m_number_panel, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    sizer->Add(m_text_panel, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(sizer);

    if (const auto number = parse_touch_number(into_u8(value)))
        m_original_suffix = number->suffix;
    wxString trimmed = value;
    const bool numeric = parse_touch_number(into_u8(value)).has_value() || (numeric_hint && trimmed.Trim().Trim(false).IsEmpty());
    set_number_mode(numeric && !password);

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        on_step(m_repeat_delta);
        if (m_repeat_timer.IsOneShot())
            m_repeat_timer.Start(80);
    });
    Bind(wxEVT_CHAR_HOOK, &TouchKeypad::on_char_hook, this);
    wxGetApp().UpdateDlgDarkUI(this);
}

wxString TouchKeypad::GetValue() const
{
    if (m_number_mode && !m_text.IsEmpty())
        return m_text + from_u8(m_suffix);
    return m_text;
}

Button* TouchKeypad::make_key(wxWindow* parent, const wxString& label, int width, bool confirm)
{
    auto* key = new Button(parent, label);
    key->SetStyle(confirm ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    key->SetFont(Label::Head_20);
    key->SetCornerRadius(FromDIP(6));
    key->SetMinSize(wxSize(FromDIP(width), FromDIP(60)));
    key->SetCanFocus(false);
    return key;
}

void TouchKeypad::build_number_panel()
{
    m_number_panel = new wxPanel(this);
    m_number_panel->SetBackgroundColour(GetBackgroundColour());
    const int gap = FromDIP(8);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* steps = new wxGridSizer(1, 4, gap, gap);
    for (int i = 0; i < 4; ++i) {
        Button* key = make_key(m_number_panel, "", 120);
        m_step_buttons[i] = key;
        key->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent& e) {
            e.Skip();
            m_repeat_delta = (i == 0 ? -m_steps.coarse : i == 1 ? -m_steps.fine : i == 2 ? m_steps.fine : m_steps.coarse);
            on_step(m_repeat_delta);
            m_repeat_timer.StartOnce(400);
        });
        key->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) { e.Skip(); m_repeat_timer.Stop(); });
        key->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent& e) { e.Skip(); m_repeat_timer.Stop(); });
        steps->Add(key, 1, wxEXPAND);
    }
    sizer->Add(steps, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

    const wxString keys[] = {"7", "8", "9", BACKSPACE, "4", "5", "6", _L("Clear"), "1", "2", "3", MINUS, "0", ".", "ABC", "%"};
    auto* pad = new wxGridSizer(4, 4, gap, gap);
    for (const wxString& label : keys) {
        Button* key = make_key(m_number_panel, label, 120);
        if (label == "%")
            m_percent_key = key;
        key->Bind(wxEVT_BUTTON, [this, label](wxCommandEvent&) {
            if (label == "ABC")
                set_number_mode(false);
            else
                on_number_key(label);
        });
        pad->Add(key, 1, wxEXPAND);
    }
    sizer->Add(pad, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    Button* cancel = make_key(m_number_panel, _L("Cancel"), 160);
    Button* ok     = make_key(m_number_panel, _L("OK"), 300, true);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    actions->Add(cancel, 1, wxEXPAND | wxRIGHT, gap);
    actions->Add(ok, 2, wxEXPAND);
    sizer->Add(actions, 0, wxEXPAND);

    m_number_panel->SetSizer(sizer);
}

void TouchKeypad::build_text_panel()
{
    m_text_panel = new wxPanel(this);
    m_text_panel->SetBackgroundColour(GetBackgroundColour());
    const int gap = FromDIP(6);
    const int key_width = 72;
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto add_row = [&](const std::vector<wxString>& labels, bool letters, Button* lead = nullptr, Button* tail = nullptr) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        if (lead != nullptr)
            row->Add(lead, 0, wxRIGHT, gap);
        for (const wxString& label : labels) {
            Button* key = make_key(m_text_panel, label, key_width);
            if (letters)
                m_letter_keys.push_back(key);
            key->Bind(wxEVT_BUTTON, [this, key](wxCommandEvent&) { on_text_key(key->GetLabel()); });
            row->Add(key, 0, wxRIGHT, gap);
        }
        if (tail != nullptr)
            row->Add(tail, 0);
        sizer->Add(row, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, gap);
    };

    Button* backspace = make_key(m_text_panel, BACKSPACE, 100);
    backspace->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_text.IsEmpty())
            m_text.RemoveLast();
        update_display();
    });
    m_shift_key = make_key(m_text_panel, SHIFT, 100);
    m_shift_key->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_shift = !m_shift;
        update_letters();
    });

    add_row({"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}, false, nullptr, backspace);
    add_row({"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}, true);
    add_row({"a", "s", "d", "f", "g", "h", "j", "k", "l"}, true);
    add_row({"z", "x", "c", "v", "b", "n", "m"}, true, m_shift_key);
    add_row({"-", "_", ".", ",", ":", "/", "@", "#", "%", "+"}, false);

    auto* bottom = new wxBoxSizer(wxHORIZONTAL);
    Button* numbers = make_key(m_text_panel, "123", 100);
    Button* clear   = make_key(m_text_panel, _L("Clear"), 110);
    Button* space   = make_key(m_text_panel, " ", 260);
    Button* cancel  = make_key(m_text_panel, _L("Cancel"), 130);
    Button* ok      = make_key(m_text_panel, _L("OK"), 150, true);
    numbers->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_number_mode(true); });
    clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_text.clear();
        update_display();
    });
    space->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_text_key(" "); });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    for (Button* key : {numbers, clear, space, cancel})
        bottom->Add(key, 0, wxRIGHT, gap);
    bottom->Add(ok, 0);
    sizer->Add(bottom, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, gap);

    m_text_panel->SetSizer(sizer);
}

void TouchKeypad::set_number_mode(bool number)
{
    if (number) {
        const auto parsed = parse_touch_number(into_u8(GetValue()));
        if (parsed) {
            m_text         = from_u8(parsed->number);
            m_suffix       = parsed->suffix;
            m_min_decimals = parsed->decimals;
            m_steps        = touch_steps(*parsed);
        } else {
            m_text.clear();
            m_suffix.clear();
            m_min_decimals = 0;
            m_steps        = {1., 10., 0};
        }
        m_fresh = true;
        update_steps();
        const bool percent = (!m_original_suffix.empty() && m_original_suffix.back() == '%') ||
                             (!m_suffix.empty() && m_suffix.back() == '%');
        m_percent_key->Show(percent);
    } else {
        m_text = GetValue();
        m_repeat_timer.Stop();
    }
    m_number_mode = number;
    m_number_panel->Show(number);
    m_text_panel->Show(!number);
    update_display();
    place_on_screen(this, GetParent());
}

void TouchKeypad::update_steps()
{
    const double   values[] = {-m_steps.coarse, -m_steps.fine, m_steps.fine, m_steps.coarse};
    for (int i = 0; i < 4; ++i) {
        const wxString magnitude = from_u8(format_touch_number(std::abs(values[i]), 0, m_steps.decimals));
        m_step_buttons[i]->SetLabel((values[i] < 0 ? MINUS : wxString("+")) + magnitude);
    }
}

void TouchKeypad::update_display()
{
    wxString shown = m_number_mode ? GetValue() : m_text;
    if (m_password)
        shown = wxString(wxUniChar(0x2022), shown.length());
    m_display->SetLabel(shown.IsEmpty() ? wxString(" ") : shown);
}

void TouchKeypad::update_letters()
{
    for (Button* key : m_letter_keys)
        key->SetLabel(m_shift ? key->GetLabel().Upper() : key->GetLabel().Lower());
    m_shift_key->SetStyle(m_shift ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    m_shift_key->SetFont(Label::Head_20);
    m_shift_key->SetMinSize(wxSize(FromDIP(100), FromDIP(60)));
    m_shift_key->Refresh();
}

void TouchKeypad::on_number_key(wxString key)
{
    if (key == "%") {
        m_suffix = (!m_suffix.empty() && m_suffix.back() == '%') ? std::string() : std::string("%");
        update_display();
        return;
    }

    if (key == BACKSPACE) {
        if (!m_text.IsEmpty())
            m_text.RemoveLast();
    } else if (key == _L("Clear")) {
        m_text.clear();
    } else if (key == MINUS) {
        m_text = m_text.StartsWith("-") ? m_text.Mid(1) : "-" + m_text;
    } else if (key == ".") {
        if (m_fresh || m_text.IsEmpty())
            m_text = "0.";
        else if (m_text == "-")
            m_text = "-0.";
        else if (!m_text.Contains("."))
            m_text += ".";
    } else if (m_fresh || m_text == "0") {
        m_text = key;
    } else if (m_text == "-0") {
        m_text = "-" + key;
    } else {
        m_text += key;
    }
    m_fresh = false;
    update_display();
}

void TouchKeypad::on_text_key(const wxString& key)
{
    m_text += key;
    if (m_shift && key.length() == 1 && wxIsalpha(key[0])) {
        m_shift = false;
        update_letters();
    }
    update_display();
}

void TouchKeypad::on_step(double delta)
{
    m_text  = from_u8(step_touch_number(into_u8(m_text), delta, m_min_decimals, m_steps));
    m_fresh = false;
    update_display();
}

void TouchKeypad::on_char_hook(wxKeyEvent& evt)
{
    const int code = evt.GetKeyCode();
    if (code == WXK_ESCAPE) {
        EndModal(wxID_CANCEL);
    } else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
        EndModal(wxID_OK);
    } else if (code == WXK_BACK) {
        if (m_number_mode)
            on_number_key(BACKSPACE);
        else if (!m_text.IsEmpty()) {
            m_text.RemoveLast();
            update_display();
        }
    } else if (code == WXK_TAB) {
        evt.Skip();
    } else if (const wxChar c = evt.GetUnicodeKey(); c != WXK_NONE && c >= ' ') {
        if (m_number_mode) {
            if (wxIsdigit(c) || c == '.')
                on_number_key(wxString(c));
            else if (c == '-')
                on_number_key(MINUS);
        } else if (!evt.ShiftDown() || wxIsalpha(c)) {
            on_text_key(evt.ShiftDown() ? wxString(c).Upper() : wxString(c).Lower());
        }
    } else {
        evt.Skip();
    }
}

bool touch_input_enabled()
{
    return wxTheApp != nullptr && wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("touch_input");
}

void edit_with_touch_keypad(wxWindow* target)
{
    if (s_keypad_open || target == nullptr)
        return;
    const bool spin  = is_spin(target);
    auto*      entry = dynamic_cast<wxTextEntry*>(target);
    if (!spin && entry == nullptr)
        return;

    wxWeakRef<wxWindow> ref(target);
    wxString            value;
    {
        s_keypad_open = true;
        struct Reset { ~Reset() { s_keypad_open = false; } } reset;
        const wxString initial = spin ? dynamic_cast<wxSpinCtrlBase*>(target)->GetTextValue() : entry->GetValue();
        TouchKeypad pad(wxGetTopLevelParent(target), initial, !spin && target->HasFlag(wxTE_PASSWORD), spin || has_numeric_validator(target));
        if (pad.ShowModal() != wxID_OK || !ref)
            return;
        value = pad.GetValue();
    }

    target->SetFocus();
    if (spin) {
        commit_spin(target, value);
        return;
    }

    // Same sequence as typing the text and pressing Enter: SetValue() emits wxEVT_TEXT, and the
    // wxEVT_TEXT_ENTER is built exactly as wxTextCtrl::OnChar() and wxSearchCtrl::OnChar() build it
    // for a real Enter key. Focus goes back first so the controls that commit on focus loss still
    // see it leave later.
    if (entry->GetValue() != value)
        entry->SetValue(value);
    if (!ref)
        return;
    entry->SetInsertionPointEnd();
    if (target->HasFlag(wxTE_PROCESS_ENTER)) {
        wxCommandEvent enter(wxEVT_TEXT_ENTER, target->GetId());
        enter.SetEventObject(target);
        enter.SetString(entry->GetValue());
        target->HandleWindowEvent(enter);
    }
}

int TouchInputFilter::FilterEvent(wxEvent& event)
{
    const wxEventType type = event.GetEventType();
    if (type != wxEVT_LEFT_DOWN && type != wxEVT_LEFT_UP)
        return Event_Skip;

    auto*         window = dynamic_cast<wxWindow*>(event.GetEventObject());
    const wxPoint pos    = static_cast<wxMouseEvent&>(event).GetPosition();
    wxWindow*     target = nullptr;
    if (window != nullptr && !s_keypad_open && touch_input_enabled())
        target = touch_target(window, pos);
    if (target == nullptr || !accepts_touch_keypad(target)) {
        if (type == wxEVT_LEFT_UP)
            m_pressed = nullptr;
        return Event_Skip;
    }

    if (type == wxEVT_LEFT_DOWN) {
        m_pressed    = window;
        m_pressed_at = pos;
        // ComboBox opens its list on any press; over its text part the press belongs to the pad.
        return dynamic_cast<::ComboBox*>(window) != nullptr ? Event_Processed : Event_Skip;
    }

    // A touch-scroll or a text selection drag also ends in a release over a field; only a tap counts.
    const int  slop = window->FromDIP(16);
    const bool tap  = m_pressed.get() == window && std::abs(pos.x - m_pressed_at.x) <= slop && std::abs(pos.y - m_pressed_at.y) <= slop;
    m_pressed = nullptr;
    if (!tap)
        return Event_Skip;

    // Let GTK finish the release (focus, cursor) before a nested modal loop starts.
    wxWeakRef<wxWindow> ref(target);
    wxTheApp->CallAfter([ref]() {
        if (ref && accepts_touch_keypad(ref.get()))
            edit_with_touch_keypad(ref.get());
    });
    return Event_Skip;
}

}} // namespace Slic3r::GUI
