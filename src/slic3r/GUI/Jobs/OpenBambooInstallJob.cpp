#include "OpenBambooInstallJob.hpp"

#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/OpenBambooPlugin.hpp"

namespace Slic3r {
namespace GUI {

OpenBambooInstallJob::OpenBambooInstallJob()
{
    name         = "plugins";
    package_name = "open-bamboo-networking";
}

void OpenBambooInstallJob::process(Ctl& ctl)
{
    AppConfig* app_config = wxGetApp().app_config;
    if (!app_config)
        return;

    auto post = [this](const wxEventType& type) {
        if (!m_event_handle)
            return;
        wxCommandEvent evt(type);
        evt.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, evt);
    };

    try {
        std::string version = OpenBambooPlugin::install_latest(
            [this, &ctl](int percent, const std::string& stage) -> bool {
                update_status(ctl, percent, stage);
                return !ctl.was_canceled();
            });

        if (ctl.was_canceled()) {
            post(wxEVT_CLOSE_WINDOW);
            return;
        }

        // Point Orca at the freshly installed plugin; mark the source so the stock
        // network-plugin update flow does not clobber it.
        app_config->set_network_plugin_version(version);
        app_config->set_bool("installed_networking", true);
        app_config->set(SETTING_NETWORK_PLUGIN_REMIND_LATER, "true");
        app_config->set("network_plugin_source", "openbamboo");
        app_config->set("network_plugin_obn_version", version);
        // Legacy vs. modern is inferred from the installed plugin version
        // (set above via set_network_plugin_version); no separate flag to update.

        // LAN-only by default (won't overwrite an existing obn.conf).
        OpenBambooPlugin::seed_lan_conf();

        GUI::wxGetApp().CallAfter([app_config] { app_config->save(); });

        update_status(ctl, 100, _u8L("Installed successfully"));
        post(EVT_UPGRADE_NETWORK_SUCCESS);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[OBN] install failed: " << e.what();
        if (ctl.was_canceled()) {
            post(wxEVT_CLOSE_WINDOW);
            return;
        }
        update_status(ctl, 0, _u8L("Download failed"));
        post(EVT_DOWNLOAD_NETWORK_FAILED);
    }
}

}} // namespace Slic3r::GUI
