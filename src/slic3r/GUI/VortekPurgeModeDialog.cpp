#include "VortekPurgeModeDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/settings.h>

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "libslic3r/VortekLog.hpp"

namespace Slic3r {
namespace GUI {

VortekPurgeModeDialog::VortekPurgeModeDialog(wxWindow* parent, Slic3r::PrimeVolumeMode mode)
    : wxDialog(parent, wxID_ANY, _L("Purge Mode Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
      m_mode(mode)
{
    VORTEK_LOG(warn, "VortekPurgeModeDialog: opened with mode " << (int)mode);

    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDlgDarkUI(this);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    // Title label
    auto title_lbl = new Label(this, _L("Purge Mode Settings"));
    title_lbl->SetFont(Label::Head_16);
    title_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    main_sizer->Add(title_lbl, 0, wxALIGN_LEFT | wxALL, FromDIP(15));

    auto cards_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Card 1: Standard
    m_standard_card = new ::StaticBox(this);
    m_standard_card->SetCornerRadius(FromDIP(8));
    m_standard_card->SetBorderWidth(FromDIP(2));
    m_standard_card->SetMinSize(wxSize(FromDIP(220), FromDIP(150)));

    auto standard_sizer = new wxBoxSizer(wxVERTICAL);
    auto std_title = new Label(m_standard_card, _L("Standard"));
    std_title->SetFont(Label::Head_14);
    std_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    standard_sizer->Add(std_title, 0, wxALIGN_LEFT | wxALL, FromDIP(10));

    auto std_desc = new Label(m_standard_card, _L("Perform full purging with speed and temperature transition for the best print quality."), LB_AUTO_WRAP);
    std_desc->SetFont(Label::Body_12);
    std_desc->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#666666")));
    standard_sizer->Add(std_desc, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_standard_card->SetSizer(standard_sizer);

    // Card 2: Prime Saving
    m_saving_card = new ::StaticBox(this);
    m_saving_card->SetCornerRadius(FromDIP(8));
    m_saving_card->SetBorderWidth(FromDIP(2));
    m_saving_card->SetMinSize(wxSize(FromDIP(220), FromDIP(150)));

    auto saving_sizer = new wxBoxSizer(wxVERTICAL);
    auto sav_title = new Label(m_saving_card, _L("Prime Saving"));
    sav_title->SetFont(Label::Head_14);
    sav_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    saving_sizer->Add(sav_title, 0, wxALIGN_LEFT | wxALL, FromDIP(10));

    auto sav_desc = new Label(m_saving_card, _L("Reduces prime waste and prints faster. May cause slight color mixing or small surface defects."), LB_AUTO_WRAP);
    sav_desc->SetFont(Label::Body_12);
    sav_desc->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#666666")));
    saving_sizer->Add(sav_desc, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_saving_card->SetSizer(saving_sizer);

    cards_sizer->Add(m_standard_card, 1, wxEXPAND | wxALL, FromDIP(10));
    cards_sizer->Add(m_saving_card, 1, wxEXPAND | wxALL, FromDIP(10));
    main_sizer->Add(cards_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(5));

    // OK and Cancel buttons
    auto dlg_btns = new DialogButtons(this, {"Confirm", "Cancel"});
    main_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(15));
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    // Click events on cards and children
    auto select_std = [this](wxMouseEvent&) {
        m_mode = Slic3r::PrimeVolumeMode::pvmDefault;
        VORTEK_LOG(warn, "VortekPurgeModeDialog: standard option selected");
        update_selection();
    };
    m_standard_card->Bind(wxEVT_LEFT_DOWN, select_std);
    std_title->Bind(wxEVT_LEFT_DOWN, select_std);
    std_desc->Bind(wxEVT_LEFT_DOWN, select_std);

    auto select_sav = [this](wxMouseEvent&) {
        m_mode = Slic3r::PrimeVolumeMode::pvmSaving;
        VORTEK_LOG(warn, "VortekPurgeModeDialog: prime saving option selected");
        update_selection();
    };
    m_saving_card->Bind(wxEVT_LEFT_DOWN, select_sav);
    sav_title->Bind(wxEVT_LEFT_DOWN, select_sav);
    sav_desc->Bind(wxEVT_LEFT_DOWN, select_sav);

    // Button actions
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        VORTEK_LOG(warn, "VortekPurgeModeDialog: confirmed with mode " << (int)m_mode);
        EndModal(wxID_OK);
    }, wxID_APPLY); // Confirm (wxID_APPLY matches DialogButtons "Confirm" action)
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); }, wxID_CANCEL);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    update_selection();
}

void VortekPurgeModeDialog::update_selection()
{
    wxColour bg_normal = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    wxColour bg_selected = StateColor::darkModeColorFor(wxColour("#E5F0EE"));
    wxColour bd_normal = StateColor::darkModeColorFor(wxColour("#DBDBDB"));
    wxColour bd_selected = StateColor::darkModeColorFor(wxColour("#009688"));

    auto update_card = [](::StaticBox* card, const wxColour& bg, const wxColour& bd) {
        card->SetBackgroundColor(bg);
        card->SetBorderColor(bd);
        for (auto child : card->GetChildren()) {
            child->SetBackgroundColour(bg);
            child->Refresh();
        }
        card->Refresh();
    };

    if (m_mode == Slic3r::PrimeVolumeMode::pvmDefault) {
        update_card(m_standard_card, bg_selected, bd_selected);
        update_card(m_saving_card, bg_normal, bd_normal);
    } else {
        update_card(m_standard_card, bg_normal, bd_normal);
        update_card(m_saving_card, bg_selected, bd_selected);
    }
}

} // namespace GUI
} // namespace Slic3r
