#ifndef __OpenBambooInstallJob_HPP__
#define __OpenBambooInstallJob_HPP__

#include "UpgradeNetworkJob.hpp"

namespace Slic3r {
namespace GUI {

// Installs the latest Open Bamboo Networking plugin from GitHub and points the
// network_plugin_version config at it. Reuses UpgradeNetworkJob's event wiring so
// DownloadProgressDialog can drive it with no extra UI plumbing.
class OpenBambooInstallJob : public UpgradeNetworkJob
{
public:
    OpenBambooInstallJob();
    void process(Ctl& ctl) override;
};

}} // namespace Slic3r::GUI

#endif // __OpenBambooInstallJob_HPP__
