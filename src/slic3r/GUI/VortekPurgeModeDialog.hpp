#ifndef slic3r_GUI_VortekPurgeModeDialog_hpp_
#define slic3r_GUI_VortekPurgeModeDialog_hpp_

#include <wx/dialog.h>
#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "Widgets/StaticBox.hpp"

class wxBoxSizer;

namespace Slic3r {
namespace GUI {

class VortekPurgeModeDialog : public wxDialog
{
public:
    VortekPurgeModeDialog(wxWindow* parent, Slic3r::PrimeVolumeMode mode);
    Slic3r::PrimeVolumeMode get_mode() const { return m_mode; }

private:
    Slic3r::PrimeVolumeMode m_mode;
    ::StaticBox* m_standard_card{ nullptr };
    ::StaticBox* m_saving_card{ nullptr };
    
    void update_selection();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_VortekPurgeModeDialog_hpp_
