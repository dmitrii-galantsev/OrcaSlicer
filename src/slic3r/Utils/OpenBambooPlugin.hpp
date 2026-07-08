#ifndef slic3r_OpenBambooPlugin_hpp_
#define slic3r_OpenBambooPlugin_hpp_

#include <functional>
#include <string>

// Open Bamboo Networking (OBN): open-source, LAN-only drop-in for the proprietary
// bambu_networking plugin (github.com/ClusterM/open-bamboo-networking). Downloads
// the latest release, picks the ABI dir matching this build, and installs the
// binaries into data_dir()/plugins/ under the versioned name Orca loads. No
// wxWidgets dependency; driven by a progress callback so it can run on a Job thread.
namespace Slic3r {
namespace OpenBambooPlugin {

// GitHub project the release is fetched from.
static const char* const OBN_REPO_OWNER = "ClusterM";
static const char* const OBN_REPO_NAME  = "open-bamboo-networking";

// percent in [0,100] + stage string; return false to cancel (install throws).
using ProgressFn = std::function<bool(int percent, const std::string& stage)>;

// Install the latest OBN release; returns the installed version string for the
// network_plugin_version config key. Throws std::runtime_error on failure.
std::string install_latest(const ProgressFn& progress);

// Write a LAN-only obn.conf (block_cloud = 1) if none exists. Never overwrites.
bool seed_lan_conf();

}} // namespace Slic3r::OpenBambooPlugin

#endif // slic3r_OpenBambooPlugin_hpp_
