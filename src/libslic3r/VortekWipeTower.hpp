#ifndef VORTEK_WIPE_TOWER_HPP
#define VORTEK_WIPE_TOWER_HPP

#include "GCode/WipeTower.hpp"

namespace Vortek {

/**
 * @brief Delegate helper functions for Slic3r::WipeTower mapping and scheduling.
 */
namespace WipeTower {

/**
 * @brief Queries if two logical filaments print on the same physical carriage.
 * 
 * @param wt Pointer to the WipeTower object
 * @param filament_id_1 First filament ID
 * @param filament_id_2 Second filament ID
 * @param layer_id 0-based layer index
 * @return True if both print on the same extruder carriage, false otherwise
 */
bool is_same_extruder(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id);

/**
 * @brief Queries if two logical filaments print from the exact same nozzle slot.
 * 
 * @param wt Pointer to the WipeTower object
 * @param filament_id_1 First filament ID
 * @param filament_id_2 Second filament ID
 * @param layer_id 0-based layer index
 * @return True if both print from the same nozzle, false otherwise
 */
bool is_same_nozzle(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id);

/**
 * @brief Resolves the physical nozzle ID (0-based slot) for a given logical filament.
 * 
 * @param wt Pointer to the WipeTower object
 * @param filament_id Logical filament ID
 * @param layer_id 0-based layer index
 * @return Physical nozzle ID
 */
int get_nozzle_id(const Slic3r::WipeTower* wt, int filament_id, int layer_id);

/**
 * @brief Resolves the physical extruder ID for a given logical filament.
 * 
 * @param wt Pointer to the WipeTower object
 * @param filament_id Logical filament ID
 * @param layer_id 0-based layer index
 * @return Physical extruder ID (usually 0 or 1)
 */
int get_extruder_id(const Slic3r::WipeTower* wt, int filament_id, int layer_id);

/**
 * @brief Checks if filament ramming is needed before toolchange on a given layer.
 * 
 * Ramming is usually skipped if we are switching between different nozzles on the same carriage.
 * 
 * @param wt Pointer to the WipeTower object
 * @param filament_id_1 Source filament ID
 * @param filament_id_2 Target filament ID
 * @param layer_id 0-based layer index
 * @return True if ramming is needed, false otherwise
 */
bool is_need_ramming(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id);

} // namespace WipeTower
} // namespace Vortek

#endif // VORTEK_WIPE_TOWER_HPP
