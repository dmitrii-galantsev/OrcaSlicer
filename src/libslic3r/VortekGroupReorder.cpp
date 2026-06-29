#include "VortekGroupReorder.hpp"
#include "VortekMultiNozzle.hpp"
#include "VortekLog.hpp"
#include <algorithm>

namespace Vortek {

bool GroupReorder::handle_nozzle_manual_reorder(
    Slic3r::Print* print,
    const Slic3r::PrintConfig* print_config,
    const std::vector<unsigned int>& used_filaments,
    std::vector<int>& filament_maps,
    unsigned int number_of_extruders)
{
    if (!print || !print_config) return false;

    // Check if we are in the nozzle manual mapping mode
    if (print_config->filament_map_mode.value != Slic3r::fmmNozzleManual) {
        return false;
    }

    VORTEK_LOG(info, "processing fmmNozzleManual reorder for " << used_filaments.size() << " used filaments");

    // 1. Build manual filament map (0-based instead of 1-based GUI representation)
    auto manual_filament_map = print_config->filament_map.values;
    std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });

    // 2. Parse stats and create layered result
    auto nozzle_stats = Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(print_config->extruder_nozzle_stats.values);
    float nozzle_dia = print_config->nozzle_diameter.values.empty() ? 0.4f : print_config->nozzle_diameter.values.front();

    auto nozzle_result = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(
        used_filaments,
        manual_filament_map,
        print_config->filament_volume_map.values,
        print_config->filament_nozzle_map.values,
        nozzle_stats,
        nozzle_dia
    );

    if (!nozzle_result) {
        VORTEK_LOG(error, "failed to build nozzle group result from filament nozzle map!");
        return false;
    }

    VORTEK_LOG(info, "nozzle group result built successfully, dynamic nozzle map = " << nozzle_result->is_support_dynamic_nozzle_map());

    // 3. Store result on print
    print->set_nozzle_group_result(std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*nozzle_result));

    // 4. Update config overrides (nozzle diameters, retracts etc.)
    print->update_to_config_by_nozzle_group_result(*nozzle_result);

    // 5. Update filament_maps for ToolOrdering output mapping
    for (size_t fid = 0; fid < number_of_extruders; ++fid) {
        filament_maps[fid] = nozzle_result->get_extruder_id(static_cast<int>(fid), -1);
        VORTEK_LOG(debug, "mapped logical filament " << fid << " to physical extruder " << filament_maps[fid]);
    }

    return true;
}
} // namespace Vortek
