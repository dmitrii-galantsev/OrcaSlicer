#include "VortekWipeTower.hpp"
#include "VortekLog.hpp"

namespace Vortek {
namespace WipeTower {

bool is_same_extruder(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id)
{
    if (wt && wt->m_multi_nozzle_group_result) {
        bool res = wt->m_multi_nozzle_group_result->are_filaments_same_extruder(filament_id_1, filament_id_2, layer_id);
        VORTEK_LOG(debug, "is_same_extruder: fil " << filament_id_1 << " vs " << filament_id_2 << " on layer " << layer_id << " -> " << res);
        return res;
    }
    return true; // fallback
}

bool is_same_nozzle(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id)
{
    if (wt && wt->m_multi_nozzle_group_result) {
        bool res = wt->m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
        VORTEK_LOG(debug, "is_same_nozzle: fil " << filament_id_1 << " vs " << filament_id_2 << " on layer " << layer_id << " -> " << res);
        return res;
    }
    return true; // fallback
}

int get_nozzle_id(const Slic3r::WipeTower* wt, int filament_id, int layer_id)
{
    if (wt && wt->m_multi_nozzle_group_result) {
        int id = wt->m_multi_nozzle_group_result->get_nozzle_id(filament_id, layer_id);
        VORTEK_LOG(debug, "get_nozzle_id: fil " << filament_id << " on layer " << layer_id << " -> " << id);
        return id;
    }
    return 0; // fallback
}

int get_extruder_id(const Slic3r::WipeTower* wt, int filament_id, int layer_id)
{
    if (wt && wt->m_multi_nozzle_group_result) {
        int id = wt->m_multi_nozzle_group_result->get_extruder_id(filament_id, layer_id);
        VORTEK_LOG(debug, "get_extruder_id: fil " << filament_id << " on layer " << layer_id << " -> " << id);
        return id;
    }
    return 0; // fallback
}

bool is_need_ramming(const Slic3r::WipeTower* wt, int filament_id_1, int filament_id_2, int layer_id)
{
    if (wt && wt->m_multi_nozzle_group_result) {
        // Ramming is needed if filaments print from different physical nozzles
        bool res = !wt->m_multi_nozzle_group_result->are_filaments_same_nozzle(filament_id_1, filament_id_2, layer_id);
        VORTEK_LOG(debug, "is_need_ramming: fil " << filament_id_1 << " vs " << filament_id_2 << " on layer " << layer_id << " -> " << res);
        return res;
    }
    return true; // fallback
}

} // namespace WipeTower
} // namespace Vortek
