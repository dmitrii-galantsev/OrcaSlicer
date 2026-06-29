#ifndef VORTEK_GROUP_REORDER_HPP
#define VORTEK_GROUP_REORDER_HPP

#include <vector>
#include <set>
#include <map>
#include <memory>
#include "Print.hpp"

namespace Vortek {

class GroupReorder {
public:

/**
 * @brief Hook called during extruder reordering to handle the manual nozzle mapping mode (fmmNozzleManual).
 * 
 * If active, it resolves the mapping between logical filaments and active physical nozzles.
 * 
 * @param print Pointer to the Print object
 * @param print_config Pointer to the PrintConfig object
 * @param used_filaments List of logical filament IDs used in the current print
 * @param filament_maps Output vector mapping logical filaments to physical extruders
 * @param number_of_extruders The total count of physical extruders configured (usually 2)
 * @return True if the manual mapping was handled, false otherwise
 */
static bool handle_nozzle_manual_reorder(
    Slic3r::Print* print,
    const Slic3r::PrintConfig* print_config,
    const std::vector<unsigned int>& used_filaments,
    std::vector<int>& filament_maps,
    unsigned int number_of_extruders);

}; // class GroupReorder
} // namespace Vortek

#endif // VORTEK_GROUP_REORDER_HPP
