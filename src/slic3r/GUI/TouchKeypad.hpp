#ifndef slic3r_GUI_TouchKeypad_hpp_
#define slic3r_GUI_TouchKeypad_hpp_

#include <optional>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/eventfilter.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>

class wxPanel;
class wxStaticText;
class Button;

namespace Slic3r { namespace GUI {

struct TouchNumber
{
    std::string number;
    double      value;
    int         decimals;
    std::string suffix;
};

struct TouchSteps
{
    double fine;
    double coarse;
    int    decimals;
};

// Accepts an optionally signed decimal with '.' as separator, optionally followed by "%" or "mm".
// Lists ("0.2,0.2"), exponents and anything else are not numbers.
std::optional<TouchNumber> parse_touch_number(const std::string& text);
TouchSteps                 touch_steps(const TouchNumber& number);
// Fixed-point with max_decimals, trailing zeros dropped down to min_decimals; never "-0".
std::string                format_touch_number(double value, int min_decimals, int max_decimals);
// An unparseable number_text counts as 0.
std::string                step_touch_number(const std::string& number_text, double delta, int min_decimals, const TouchSteps& steps);

class TouchKeypad : public wxDialog
{
public:
    TouchKeypad(wxWindow* parent, const wxString& value, bool password, bool numeric_hint);

    wxString GetValue() const;

private:
    void build_number_panel();
    void build_text_panel();
    void set_number_mode(bool number);
    void update_steps();
    void update_display();
    void update_letters();

    void on_number_key(wxString key);
    void on_text_key(const wxString& key);
    void on_step(double delta);
    void on_char_hook(wxKeyEvent& evt);

    Button* make_key(wxWindow* parent, const wxString& label, int width, bool confirm = false);

    bool             m_password;
    bool             m_number_mode { false };
    bool             m_fresh { true };
    bool             m_shift { false };
    wxString         m_text;
    std::string      m_suffix;
    std::string      m_original_suffix;
    int              m_min_decimals { 0 };
    TouchSteps       m_steps { 1., 10., 0 };

    wxStaticText*         m_display { nullptr };
    wxPanel*              m_number_panel { nullptr };
    wxPanel*              m_text_panel { nullptr };
    Button*               m_step_buttons[4] {};
    Button*               m_percent_key { nullptr };
    std::vector<Button*>  m_letter_keys;
    Button*               m_shift_key { nullptr };

    wxTimer m_repeat_timer;
    double  m_repeat_delta { 0. };
};

// Opens the keypad for ctrl and, on OK, commits the result the same way pressing Enter would.
void edit_with_touch_keypad(wxTextCtrl* ctrl);

// Installed on the application: a tap on any editable single-line wxTextCtrl opens the keypad
// while the "touch_input" app config option is on.
class TouchInputFilter : public wxEventFilter
{
public:
    int FilterEvent(wxEvent& event) override;

private:
    wxWeakRef<wxTextCtrl> m_pressed;
    wxPoint               m_pressed_at;
};

}} // namespace Slic3r::GUI

#endif
