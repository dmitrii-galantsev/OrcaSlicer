#pragma once
#include "DevDefs.h"
#include "DevNozzleSystem.h"
#include "DevFilaSystem.h"

#include "libslic3r/VortekMultiNozzle.hpp"
#include <unordered_map>

namespace Slic3r
{
class MachineObject;

namespace GUI
{
class Plater;
}
}; // namespace Slic3r::GUI

namespace Slic3r
{

class VortekUtilBackend
{
public:
    VortekUtilBackend() = delete;

public:
    static MultiNozzleUtils::NozzleInfo GetNozzleInfo(const DevNozzle& dev_nozzle);

    // for rack
    static std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> GetNozzleGroupResult(Slic3r::GUI::Plater *plater);
    static std::unordered_map<NozzleDef, int> CollectNozzleInfo(MultiNozzleUtils::NozzleGroupResultBase *nozzle_group_res, int logic_ext_id);

    // for filament preset
    static std::optional<DevFilamentDryingPreset> GetFilamentDryingPreset(const std::string& fila_id);
};

}; // namespace Slic3r
