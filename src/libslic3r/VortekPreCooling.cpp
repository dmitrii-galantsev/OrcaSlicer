#include "VortekPreCooling.hpp"
#include "VortekLog.hpp"
#include "GCodeReader.hpp"
#include "Print.hpp"
#include <regex>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <limits>

namespace Vortek {

PreCooling::PreCooling(
    const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves,
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result,
    const std::vector<int>& filament_nozzle_temps,
    const std::vector<int>& filament_nozzle_temps_initial_layer,
    const std::vector<int>& physical_extruder_map,
    int valid_machine_id,
    float inject_time_threshold,
    bool handle_hotend_as_extruder,
    bool has_filament_switcher,
    int standby_temp_delta,
    const std::vector<int>& pre_cooling_temp_nc,
    const std::vector<int>& filament_idle_temps,
    const std::vector<double>& cooling_rate,
    const std::vector<double>& heating_rate,
    const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks,
    const std::vector<int>& extruder_max_nozzle_count,
    const std::vector<double>& filament_preheat_temperature_delta,
    const std::vector<double>& filament_max_temperature_drop_when_ec,
    unsigned int machine_start_gcode_end_id,
    unsigned int machine_end_gcode_start_id,
    const std::vector<Slic3r::ExtruderType>& extruder_types,
    const std::vector<double>& nozzle_diameter
) :
    m_moves(moves),
    m_nozzle_group_result(nozzle_group_result),
    m_filament_nozzle_temps(filament_nozzle_temps),
    m_filament_nozzle_temps_initial_layer(filament_nozzle_temps_initial_layer),
    m_physical_extruder_map(physical_extruder_map),
    m_valid_machine_id(valid_machine_id),
    m_inject_time_threshold(inject_time_threshold),
    m_handle_hotend_as_extruder(handle_hotend_as_extruder),
    m_has_filament_switcher(has_filament_switcher),
    m_standby_temp_delta(standby_temp_delta),
    m_filament_pre_cooling_temps_nc(pre_cooling_temp_nc),
    m_filament_idle_temps(filament_idle_temps),
    m_cooling_rate(cooling_rate),
    m_heating_rate(heating_rate),
    m_skippable_blocks(skippable_blocks),
    m_extruder_max_nozzle_count(extruder_max_nozzle_count),
    m_filament_preheat_temperature_delta(filament_preheat_temperature_delta),
    m_filament_max_temperature_drop_when_ec(filament_max_temperature_drop_when_ec),
    m_machine_start_gcode_end_id(machine_start_gcode_end_id),
    m_machine_end_gcode_start_id(machine_end_gcode_start_id),
    m_extruder_types(extruder_types),
    m_nozzle_diameter(nozzle_diameter)
{
    std::sort(m_moves.begin(), m_moves.end(), [](const auto& a, const auto& b) {
        return a.gcode_id < b.gcode_id;
    });
}

void PreCooling::process_pre_cooling_and_heating(InsertedLinesMap& inserted_operation_lines)
{
    VORTEK_LOG(info, "process_pre_cooling_and_heating: blocks count = " << m_extruder_free_blocks.size());
    bool is_multiple_nozzle = std::any_of(m_extruder_max_nozzle_count.begin(), m_extruder_max_nozzle_count.end(), [](auto& elem) { return elem > 1; });
    auto get_nozzle_temp = [this, is_multiple_nozzle](int filament_id, bool is_first_layer, bool from_or_to, bool consider_preheat_temperature_delta) {
        if (filament_id == -1)
            return from_or_to ? 140 : 0;
        double temp = (is_first_layer ? m_filament_nozzle_temps_initial_layer[filament_id] : m_filament_nozzle_temps[filament_id]);
        if (consider_preheat_temperature_delta)
            return (int)(temp - m_filament_preheat_temperature_delta[filament_id]);
        else
            return (int)(temp);
    };

    bool has_mixed_extruder_types = m_extruder_types.size() > 1 &&
        std::adjacent_find(m_extruder_types.begin(), m_extruder_types.end(), std::not_equal_to<>()) != m_extruder_types.end();
    
    float first_nozzle_dia = m_nozzle_diameter.empty() ? 0.4 : m_nozzle_diameter.front();
    float switcher_temp_offset = (first_nozzle_dia >= 0.6 - 1e-5) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;
    for (auto& block : m_extruder_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_free_blocks) {
        int extruder_id = elem.first;
        auto& extruder_free_blocks = elem.second;
        for (auto iter = extruder_free_blocks.begin(); iter != extruder_free_blocks.end(); ++iter) {
            bool is_end = std::next(iter) == extruder_free_blocks.end();
            bool apply_pre_cooling = true;
            bool apply_pre_heating = is_end ? false : true;
            float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
            float target_temp = get_nozzle_temp(iter->next_filament_id, false, false, !iter->ignore_cooling_before_tower);
            
            if (m_has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                float print_temp = get_nozzle_temp(iter->next_filament_id, false, false, false);
                target_temp = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating);
        }
    }
}

void PreCooling::build_extruder_free_blocks(
    const std::vector<FilamentUsageBlock>& filament_usage_blocks,
    const std::vector<ExtruderUsageBlock>& extruder_usage_blocks
)
{
    if (extruder_usage_blocks.size() <= 1)
        build_by_filament_blocks(filament_usage_blocks);
    else
        build_by_extruder_blocks(extruder_usage_blocks);
}

void PreCooling::build_by_filament_blocks(const std::vector<FilamentUsageBlock>& filament_usage_blocks)
{
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<FilamentUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : filament_usage_blocks) {
        per_extruder_usage_blocks[block.nozzle_id].emplace_back(block);
    }

    FilamentUsageBlock start_filament_block(-1, -1, -1, 0, m_machine_start_gcode_end_id);
    FilamentUsageBlock end_filament_block(-1, -1, -1, m_machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());

    for (auto& elem : per_extruder_usage_blocks) {
        auto &blocks = elem.second;
        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t nozzle_id = elem.first;
        const auto& filament_blocks = elem.second;

        for (auto iter = filament_blocks.begin(); iter < filament_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == filament_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->upper_gcode_id;
            block.last_filament_id = iter->filament_id;
            block.last_nozzle_id = iter->nozzle_id;
            block.free_upper_gcode_id = niter->lower_gcode_id;
            block.next_filament_id = niter->filament_id;
            block.next_nozzle_id = niter->nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = nozzle_id;
            block.partial_free_lower_id = block.free_lower_gcode_id;
            block.partial_free_upper_id = block.free_lower_gcode_id;
            m_extruder_free_blocks.emplace_back(block);
        }
    }
    std::for_each(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](ExtruderFreeBlock &block) { block.ignore_cooling_before_tower = true; });
    std::sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

void PreCooling::build_by_extruder_blocks(const std::vector<ExtruderUsageBlock>& extruder_usage_blocks)
{
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<ExtruderUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : extruder_usage_blocks)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        auto& blocks = elem.second;
        ExtruderUsageBlock start_filament_block;
        start_filament_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_filament_block.initialize_step_2(m_machine_start_gcode_end_id);
        start_filament_block.initialize_step_3(m_machine_start_gcode_end_id, -1, m_machine_start_gcode_end_id, -1);

        ExtruderUsageBlock end_filament_block;
        end_filament_block.initialize_step_1(extruder_id, m_machine_end_gcode_start_id, -1, -1);
        end_filament_block.initialize_step_2(std::numeric_limits<int>::max());
        end_filament_block.initialize_step_3(std::numeric_limits<int>::max(), -1, std::numeric_limits<int>::max(), -1);

        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& usage_blocks = elem.second;
        for (auto iter = usage_blocks.begin(); iter != usage_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == usage_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->end_id;
            block.last_filament_id = iter->end_filament;
            block.last_nozzle_id = iter->end_nozzle_id;
            block.free_upper_gcode_id = niter->start_id;
            block.next_filament_id = niter->start_filament;
            block.next_nozzle_id = niter->start_nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = iter->post_extrusion_start_id;
            block.partial_free_upper_id = iter->post_extrusion_end_id;
            block.ignore_cooling_before_tower = niter->ignore_cooling_before_tower;
            m_extruder_free_blocks.emplace_back(block);
        }
    }

    std::sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

void PreCooling::inject_cooling_heating_command(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
{
    int standby_temp = 180;
    if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_pre_cooling_temps_nc.size()) {
        standby_temp = m_filament_pre_cooling_temps_nc[block.last_filament_id];
    }

    VORTEK_LOG(warning, "inject_cooling_heating_command: extruder=" << block.extruder_id
                      << " standby_temp=" << standby_temp << " curr_temp=" << curr_temp
                      << " target_temp=" << target_temp << " (always routing to BBS mode)");

    // TODO: inject_cooling_heating_command_orca is disabled due to known logic/holding issues.
    // Always use BBS preheat timeline.
    inject_cooling_heating_command_bbs(inserted_operation_lines, block, curr_temp, target_temp, pre_cooling, pre_heating);
}

void PreCooling::inject_cooling_heating_command_bbs(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
{
    VORTEK_LOG(warning, "inject_cooling_heating_command_bbs: extruder " << block.extruder_id 
                      << ", curr_temp = " << curr_temp << ", target_temp = " << target_temp 
                      << ", pre_cooling = " << pre_cooling << ", pre_heating = " << pre_heating);
    auto get_valid_extruder_id = [&](int last_nozzle_id) {
        auto nozzle_opt = m_nozzle_group_result.get_nozzle_from_id(last_nozzle_id);
        return nozzle_opt ? nozzle_opt->extruder_id : 0;
    };

    bool is_nozzle_change = (block.last_nozzle_id != block.next_nozzle_id) || (block.next_nozzle_id == -1);

    auto is_pre_cooling_valid = [this, is_nozzle_change](int idx) -> bool {
        if (idx < 0 || idx >= (int)m_filament_nozzle_temps.size())
            return false;
        if (is_nozzle_change) {
            if (idx >= (int)m_filament_pre_cooling_temps_nc.size())
                return false;
            return m_filament_pre_cooling_temps_nc[idx] > 0 && m_filament_pre_cooling_temps_nc[idx] < m_filament_nozzle_temps[idx];
        } else {
            return m_standby_temp_delta < 0;
        }
    };

    auto get_partial_free_cooling_thres = [this, is_nozzle_change](int idx) -> float {
        if (idx < 0 || idx >= (int)m_filament_nozzle_temps.size())
            return 30.f;
        if (is_nozzle_change) {
            if (idx >= (int)m_filament_pre_cooling_temps_nc.size())
                return 30.f;
            float temp_in_tower = m_filament_nozzle_temps[idx];
            return temp_in_tower - (float)(m_filament_pre_cooling_temps_nc[idx]);
        } else {
            return (float)(-m_standby_temp_delta);
        }
    };

    auto gcode_move_comp = [](const Slic3r::GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
        return a.gcode_id < gcode_id;
    };

    auto find_skip_block_end = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->second;
        }
        return 0;
    };

    auto find_skip_block_start = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };

    auto adjust_iter = [&](std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator iter,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& begin,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& end,
                           bool forward) -> std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator
    {
        if (forward) {
            while (iter != end) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_end_val = find_skip_block_end(current_id);
                if (skip_block_end_val == 0)
                    break;
                iter = std::lower_bound(iter, end, skip_block_end_val + 1, gcode_move_comp);
            }
        }
        else {
            while (iter != begin) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_start_val = find_skip_block_start(current_id);
                if (skip_block_start_val == 0)
                    break;
                auto new_iter = std::lower_bound(begin, iter, skip_block_start_val, gcode_move_comp);
                if (new_iter == begin)
                    break;
                iter = std::prev(new_iter);
            }
        }
        return iter;
    };

    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id) {
        return;
    }

    // Calculate base pre-cooling temperature used for math:
    // 1. For nozzle changes (is_nozzle_change = true), use filament_pre_cooling_temperature_nc (usually 180C).
    //    H2C firmware preheats nozzles to 180C physically during tool swap, so the preheat planner
    //    should calculate heating duration from 180C up to the print temperature (e.g., 220C).
    // 2. For regular filament changes on the same head (is_nozzle_change = false), use
    //    nozzle temperature plus standby_temperature_delta (usually 220 - 5 = 215C).
    //    This delays the preheating command, minimizing oozing before printing.
    int base_pre_cooling = 180;
    if (is_nozzle_change) {
        if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_pre_cooling_temps_nc.size()) {
            base_pre_cooling = m_filament_pre_cooling_temps_nc[block.last_filament_id];
        }
    } else {
        if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_nozzle_temps.size()) {
            base_pre_cooling = m_filament_nozzle_temps[block.last_filament_id] + m_standby_temp_delta;
        }
    }

    // Retrieve standby idle temperature from print configuration.
    int idle_temp = 0;
    if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_idle_temps.size()) {
        idle_temp = m_filament_idle_temps[block.last_filament_id];
    }

    float cooldown_temp = 25.f;
    float standby_temp = 25.f;

    // Split idle temperature handling (physical cooling target vs virtual math preheat start):
    if (idle_temp > 0) {
        // If standby idle temperature is enabled (> 0), the nozzle physically stays at idle_temp,
        // and preheat calculations start from this level.
        cooldown_temp = idle_temp;
        standby_temp = idle_temp;
    } else {
        // If idle_temp is 0 (full cooldown mode):
        // 1. cooldown_temp = 25C: the nozzle physically cools down to room temperature
        //    (M104 S25 is emitted) to prevent clogs and oozing during long idle pauses.
        // 2. standby_temp = base_pre_cooling (180C or 215C): the preheat planner virtually assumes
        //    heating starts from 180C/215C. This ensures the heater turns on at the latest possible moment.
        cooldown_temp = 25.f;
        standby_temp = base_pre_cooling;
    }

    int extruder_id = get_valid_extruder_id(block.last_nozzle_id);
    float ext_heating_rate = m_heating_rate.size() > (size_t)extruder_id ? m_heating_rate[extruder_id] : 2.0f;
    float ext_cooling_rate = m_cooling_rate.size() > (size_t)extruder_id ? m_cooling_rate[extruder_id] : 0.5f;

    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, int type, const std::string& comment = std::string()) {
        auto format_line_M104 = [&](int target_extruder_inner, int target_temp_inner, int target_filament_inner, bool skippable_inner, int next_filament_idx_inner, int next_nozzle_id_inner, const std::string& comment_inner) -> std::vector<std::string> {
            std::vector<std::string> buffer;
            if (skippable_inner) {
                const bool support_dynamic_nozzle_map = m_nozzle_group_result.is_support_dynamic_nozzle_map();
                std::string m632_line = "M632 S" + std::to_string(next_filament_idx_inner);
                if (support_dynamic_nozzle_map)
                    m632_line += " H" + std::to_string(next_nozzle_id_inner);
                if (m_extruder_max_nozzle_count.size() > (size_t)target_extruder_inner && m_extruder_max_nozzle_count[target_extruder_inner] > 1)
                    m632_line += " N R";
                m632_line += " W\n";
                buffer.emplace_back(std::move(m632_line));
            }
            buffer.emplace_back("M400\n");
            std::string M104_line = "M104";
            if (m_handle_hotend_as_extruder) {
                M104_line += (" I" + std::to_string(target_filament_inner == -1 ? next_filament_idx_inner : target_filament_inner));
            }
            else if (target_extruder_inner != -1 && target_extruder_inner < (int)m_physical_extruder_map.size()) {
                M104_line += (" T" + std::to_string(m_physical_extruder_map[target_extruder_inner]));
            }

            M104_line += " S" + std::to_string(target_temp_inner);
            M104_line += " N0";

            if (!comment_inner.empty())
                M104_line += " ;" + comment_inner;
            M104_line += '\n';

            buffer.emplace_back(M104_line);

            if (skippable_inner)
                buffer.emplace_back("M633\n");

            return buffer;
        };

        std::vector<std::string> formatted = format_line_M104(target_extruder, target_temp, target_filament, skippable, next_filament_idx, next_nozzle_id, comment);
        for (auto& line : formatted) {
            std::string log_line = line;
            if (!log_line.empty() && log_line.back() == '\n') log_line.pop_back();
            VORTEK_LOG(warning, "inject_bbs: GCODE_ID=" << gcode_id << " LINE=" << log_line << " COMMENT=" << comment);
            inserted_operation_lines[gcode_id].emplace_back(line, type);
        }
    };

    if (pre_cooling && !pre_heating) {
        if (standby_temp >= curr_temp)
            return;
        VORTEK_LOG(warning, "inject_bbs: parking nozzle ext=" << extruder_id
                            << " curr=" << curr_temp << " standby=" << standby_temp
                            << " at gcode_id=" << block.free_lower_gcode_id);
        add_M104_lines(block.free_lower_gcode_id, extruder_id, (int)cooldown_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
        return;
    }

    auto move_iter_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_lower_gcode_id, gcode_move_comp);
    auto move_iter_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_upper_gcode_id, gcode_move_comp);

    if (move_iter_lower == m_moves.cend() || move_iter_upper == m_moves.cbegin()) {
        return;
    }
    --move_iter_upper;

    float complete_free_time_gap = 0;
    if (move_iter_lower == m_moves.cbegin())
        complete_free_time_gap = move_iter_upper->time[m_valid_machine_id];
    else
        complete_free_time_gap = move_iter_upper->time[m_valid_machine_id] - std::prev(move_iter_lower)->time[m_valid_machine_id];

    auto partial_free_move_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_lower_id, gcode_move_comp);
    auto partial_free_move_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == m_moves.cend() || partial_free_move_upper == m_moves.cbegin()) {
        return;
    }
    --partial_free_move_upper;

    float partial_free_time_gap = 0;
    if (partial_free_move_lower == m_moves.cbegin())
        partial_free_time_gap = partial_free_move_upper->time[m_valid_machine_id];
    else
        partial_free_time_gap = partial_free_move_upper->time[m_valid_machine_id] - std::prev(partial_free_move_lower)->time[m_valid_machine_id];

    if (move_iter_lower >= move_iter_upper) {
        return;
    }

    bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;

    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < m_inject_time_threshold) {
        return;
    }

    if (!apply_cooling_when_partial_free && complete_free_time_gap < m_inject_time_threshold) {
        return;
    }

    if (apply_cooling_when_partial_free) {
        float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id), partial_free_time_gap * ext_cooling_rate));
        curr_temp = std::max(standby_temp, curr_temp - max_cooling_temp);
        add_M104_lines(block.partial_free_lower_id, extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling in post extrusion");
    }

    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = move_iter_upper->time[m_valid_machine_id] - (target_temp - curr_temp) / ext_heating_rate;
        std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator heating_move_iter = move_iter_lower;
        for (auto it = move_iter_lower; it != move_iter_upper + 1; ++it) {
            if (it->time[m_valid_machine_id] >= heating_start_time) {
                heating_move_iter = it;
                break;
            }
        }
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        return;
    }

    float mid_temp = std::max(standby_temp, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = move_iter_upper->time[m_valid_machine_id] - heating_temp / ext_heating_rate;
    std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator heating_move_iter = move_iter_lower;
    for (auto it = move_iter_lower; it != move_iter_upper + 1; ++it) {
        if (it->time[m_valid_machine_id] >= heating_start_time) {
            heating_move_iter = it;
            break;
        }
    }
    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);

    float real_cooling_time = heating_move_iter->time[m_valid_machine_id] - move_iter_lower->time[m_valid_machine_id];
    int real_delta_temp = std::min((int)(real_cooling_time * ext_cooling_rate), (int)curr_temp);
    if (real_delta_temp == 0)
        return;
    int cooling_temp = std::max((int)cooldown_temp, (int)curr_temp - real_delta_temp);
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
}

void PreCooling::inject_cooling_heating_command_orca(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
{
    VORTEK_LOG(warning, "inject_cooling_heating_command_orca: extruder " << block.extruder_id 
                      << ", curr_temp = " << curr_temp << ", target_temp = " << target_temp 
                      << ", pre_cooling = " << pre_cooling << ", pre_heating = " << pre_heating);
    auto get_valid_extruder_id = [&](int last_nozzle_id) {
        auto nozzle_opt = m_nozzle_group_result.get_nozzle_from_id(last_nozzle_id);
        return nozzle_opt ? nozzle_opt->extruder_id : 0;
    };

    bool is_nozzle_change = (block.last_nozzle_id != block.next_nozzle_id) || (block.next_nozzle_id == -1);

    auto is_pre_cooling_valid = [this, is_nozzle_change](int idx) -> bool {
        if (idx < 0 || idx >= (int)m_filament_nozzle_temps.size())
            return false;
        if (is_nozzle_change) {
            if (idx >= (int)m_filament_pre_cooling_temps_nc.size())
                return false;
            return m_filament_pre_cooling_temps_nc[idx] > 0 && m_filament_pre_cooling_temps_nc[idx] < m_filament_nozzle_temps[idx];
        } else {
            return m_standby_temp_delta < 0;
        }
    };

    auto get_partial_free_cooling_thres = [this, is_nozzle_change](int idx) -> float {
        if (idx < 0 || idx >= (int)m_filament_nozzle_temps.size())
            return 30.f;
        if (is_nozzle_change) {
            if (idx >= (int)m_filament_pre_cooling_temps_nc.size())
                return 30.f;
            float temp_in_tower = m_filament_nozzle_temps[idx];
            return temp_in_tower - (float)(m_filament_pre_cooling_temps_nc[idx]);
        } else {
            return (float)(-m_standby_temp_delta);
        }
    };

    auto gcode_move_comp = [](const Slic3r::GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
        return a.gcode_id < gcode_id;
    };

    auto find_skip_block_end = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->second;
        }
        return 0;
    };

    auto find_skip_block_start = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };

    auto adjust_iter = [&](std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator iter,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& begin,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& end,
                           bool forward) -> std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator
    {
        if (forward) {
            while (iter != end) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_end_val = find_skip_block_end(current_id);
                if (skip_block_end_val == 0)
                    break;
                iter = std::lower_bound(iter, end, skip_block_end_val + 1, gcode_move_comp);
            }
        }
        else {
            while (iter != begin) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_start_val = find_skip_block_start(current_id);
                if (skip_block_start_val == 0)
                    break;
                auto new_iter = std::lower_bound(begin, iter, skip_block_start_val, gcode_move_comp);
                if (new_iter == begin)
                    break;
                iter = std::prev(new_iter);
            }
        }
        return iter;
    };

    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id) {
        return;
    }

    constexpr float room_temperature = 25.f;

    int extruder_id = get_valid_extruder_id(block.last_nozzle_id);
    float ext_heating_rate = m_heating_rate.size() > (size_t)extruder_id ? m_heating_rate[extruder_id] : 2.0f;
    float ext_cooling_rate = m_cooling_rate.size() > (size_t)extruder_id ? m_cooling_rate[extruder_id] : 0.5f;

    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, int type, const std::string& comment = std::string()) {
        auto format_line_M104 = [&](int target_extruder_inner, int target_temp_inner, int target_filament_inner, bool skippable_inner, int next_filament_idx_inner, int next_nozzle_id_inner, const std::string& comment_inner) -> std::vector<std::string> {
            std::vector<std::string> buffer;
            if (skippable_inner) {
                const bool support_dynamic_nozzle_map = m_nozzle_group_result.is_support_dynamic_nozzle_map();
                std::string m632_line = "M632 S" + std::to_string(next_filament_idx_inner);
                if (support_dynamic_nozzle_map)
                    m632_line += " H" + std::to_string(next_nozzle_id_inner);
                if (m_extruder_max_nozzle_count.size() > (size_t)target_extruder_inner && m_extruder_max_nozzle_count[target_extruder_inner] > 1)
                    m632_line += " N R";
                m632_line += " W\n";
                buffer.emplace_back(std::move(m632_line));
            }
            buffer.emplace_back("M400\n");
            std::string M104_line = "M104";
            if (m_handle_hotend_as_extruder) {
                M104_line += (" I" + std::to_string(target_filament_inner == -1 ? next_filament_idx_inner : target_filament_inner));
            }
            else if (target_extruder_inner != -1 && target_extruder_inner < (int)m_physical_extruder_map.size()) {
                M104_line += (" T" + std::to_string(m_physical_extruder_map[target_extruder_inner]));
            }

            M104_line += " S" + std::to_string(target_temp_inner);
            M104_line += " N0";

            if (!comment_inner.empty())
                M104_line += " ;" + comment_inner;
            M104_line += '\n';

            buffer.emplace_back(M104_line);

            if (skippable_inner)
                buffer.emplace_back("M633\n");

            return buffer;
        };

        std::vector<std::string> formatted = format_line_M104(target_extruder, target_temp, target_filament, skippable, next_filament_idx, next_nozzle_id, comment);
        for (auto& line : formatted) {
            std::string log_line = line;
            if (!log_line.empty() && log_line.back() == '\n') log_line.pop_back();
            VORTEK_LOG(warning, "inject_orca: GCODE_ID=" << gcode_id << " LINE=" << log_line << " COMMENT=" << comment);
            inserted_operation_lines[gcode_id].emplace_back(line, type);
        }
    };

    int standby_temp = 180;
    if (is_nozzle_change) {
        if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_pre_cooling_temps_nc.size()) {
            standby_temp = m_filament_pre_cooling_temps_nc[block.last_filament_id];
        }
    } else {
        if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_nozzle_temps.size()) {
            standby_temp = m_filament_nozzle_temps[block.last_filament_id] + m_standby_temp_delta;
        }
    }
    float min_allowed_temp = std::max(room_temperature, (float)standby_temp);

    if (pre_cooling && !pre_heating) {
        if (block.free_lower_gcode_id >= 4000000000) {
            standby_temp = std::max((int)room_temperature, (int)target_temp);
        }
        if (standby_temp >= curr_temp)
            return;

        int clamped_target = standby_temp;
        VORTEK_LOG(warning, "inject: parking nozzle ext=" << extruder_id
                            << " curr=" << curr_temp << " standby=" << clamped_target
                            << " at gcode_id=" << block.free_lower_gcode_id);
        add_M104_lines(block.free_lower_gcode_id, extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
        return;
    }

    auto move_iter_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_lower_gcode_id, gcode_move_comp);
    auto move_iter_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_upper_gcode_id, gcode_move_comp);

    if (move_iter_lower == m_moves.cend() || move_iter_upper == m_moves.cbegin()) {
        return;
    }
    --move_iter_upper;

    float complete_free_time_gap = 0;
    if (move_iter_lower == m_moves.cbegin())
        complete_free_time_gap = move_iter_upper->time[m_valid_machine_id];
    else
        complete_free_time_gap = move_iter_upper->time[m_valid_machine_id] - std::prev(move_iter_lower)->time[m_valid_machine_id];

    auto partial_free_move_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_lower_id, gcode_move_comp);
    auto partial_free_move_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == m_moves.cend() || partial_free_move_upper == m_moves.cbegin()) {
        return;
    }
    --partial_free_move_upper;

    float partial_free_time_gap = 0;
    if (partial_free_move_lower == m_moves.cbegin())
        partial_free_time_gap = partial_free_move_upper->time[m_valid_machine_id];
    else
        partial_free_time_gap = partial_free_move_upper->time[m_valid_machine_id] - std::prev(partial_free_move_lower)->time[m_valid_machine_id];

    if (move_iter_lower >= move_iter_upper) {
        return;
    }

    bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;

    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < m_inject_time_threshold) {
        return;
    }

    if (!apply_cooling_when_partial_free && complete_free_time_gap < m_inject_time_threshold) {
        return;
    }

    if (apply_cooling_when_partial_free) {
        float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id), partial_free_time_gap * ext_cooling_rate));
        curr_temp = std::max(min_allowed_temp, curr_temp - max_cooling_temp);
        add_M104_lines(block.partial_free_lower_id, extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling in post extrusion");
    }

    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = move_iter_upper->time[m_valid_machine_id] - (target_temp - curr_temp) / ext_heating_rate;
        std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator heating_move_iter = move_iter_lower;
        for (auto it = move_iter_lower; it != move_iter_upper + 1; ++it) {
            if (it->time[m_valid_machine_id] >= heating_start_time) {
                heating_move_iter = it;
                break;
            }
        }
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        return;
    }

    if (target_temp <= min_allowed_temp) {
        if (pre_cooling && curr_temp > min_allowed_temp) {
            VORTEK_LOG(warning, "inject: standby(" << min_allowed_temp << ") >= target(" << target_temp
                                << "), cooling to standby only");
            add_M104_lines(block.free_lower_gcode_id, extruder_id, (int)min_allowed_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
        }
        return;
    }

    float mid_temp = std::max(min_allowed_temp, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = move_iter_upper->time[m_valid_machine_id] - heating_temp / ext_heating_rate;
    std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator heating_move_iter = move_iter_lower;
    for (auto it = move_iter_lower; it != move_iter_upper + 1; ++it) {
        if (it->time[m_valid_machine_id] >= heating_start_time) {
            heating_move_iter = it;
            break;
        }
    }
    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);

    float real_cooling_time = heating_move_iter->time[m_valid_machine_id] - move_iter_lower->time[m_valid_machine_id];
    int real_delta_temp = std::min((int)(real_cooling_time * ext_cooling_rate), (int)curr_temp);
    if (real_delta_temp == 0)
        return;
    int cooling_temp = std::max((int)min_allowed_temp, (int)curr_temp - real_delta_temp);
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
}

PreCooling::InsertedLinesMap PreCooling::run_pre_scan(Slic3r::GCodeProcessor& processor, const std::string& filename)
{
    VORTEK_LOG(info, "run_pre_scan started on file: " << filename);
    InsertedLinesMap inserted_operation_lines;
    if (!processor.m_print || !processor.m_print->get_layered_nozzle_group_result()) {
        VORTEK_LOG(warning, "run_pre_scan: print or layered nozzle group result is null!");
        return inserted_operation_lines;
    }

    const Slic3r::PrintConfig& print_config = processor.m_print->config();
    const auto& nozzle_group = *processor.m_print->get_layered_nozzle_group_result();

    std::vector<FilamentUsageBlock> filament_blocks;
    std::vector<ExtruderUsageBlock> extruder_blocks = { ExtruderUsageBlock() };
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks;

    unsigned int machine_start_gcode_end_line_id = 0;
    unsigned int machine_end_gcode_start_line_id = std::numeric_limits<unsigned int>::max();

    int current_layer_id = 0;
    unsigned int line_id = 0;

    auto handle_nozzle_change_line = [&](const std::string& line, int& old_filament, int& next_filament, int& extruder_id, int& old_nozzle_id, int& new_nozzle_id) -> bool {
        std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        old_filament = std::stoi(match[1]);
        next_filament = std::stoi(match[2]);
        old_nozzle_id = std::stoi(match[3]);
        new_nozzle_id = std::stoi(match[4]);
        auto nozzle_opt = nozzle_group.get_nozzle_from_id(new_nozzle_id);
        extruder_id = nozzle_opt ? nozzle_opt->extruder_id : -1;
        return true;
    };

    auto handle_filament_change = [&](int filament_id, int current_line_id, int nozzle_id = -1) {
        VORTEK_LOG(warning, "handle_filament_change: fid=" << filament_id 
            << ", line=" << current_line_id 
            << ", start_end=" << machine_start_gcode_end_line_id 
            << ", end_start=" << machine_end_gcode_start_line_id);
        if (static_cast<unsigned int>(current_line_id) < machine_start_gcode_end_line_id || 
            static_cast<unsigned int>(current_line_id) > machine_end_gcode_start_line_id) {
            VORTEK_LOG(warning, "handle_filament_change: skipped due to start/end boundaries");
            return;
        }
        if (!filament_blocks.empty())
            filament_blocks.back().upper_gcode_id = current_line_id;
        if (nozzle_id == -1) {
            nozzle_id = nozzle_group.get_nozzle_id(filament_id, current_layer_id);
            VORTEK_LOG(warning, "handle_filament_change: nozzle_id evaluated to " << nozzle_id << " from nozzle_group");
        }
        int extruder_id = 0;
        auto nozzle_ptr = nozzle_group.get_nozzle_from_id(nozzle_id);
        if (nozzle_ptr)
            extruder_id = nozzle_ptr->extruder_id;
        filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, current_line_id, -1);
        VORTEK_LOG(warning, "handle_filament_change: added block: fid=" << filament_id << ", ext=" << extruder_id << ", nozzle=" << nozzle_id);
    };

    Slic3r::GCodeReader parser;
    parser.parse_file(filename, [&](Slic3r::GCodeReader& reader, const Slic3r::GCodeReader::GCodeLine& line) {
        ++line_id;
        const std::string& raw_line = line.raw();

        // Detect end of start G-code (before first layer Change/Height)
        if (machine_start_gcode_end_line_id == 0 && 
            (raw_line.find("CHANGE_LAYER") != std::string::npos || raw_line.find("Z_HEIGHT") != std::string::npos)) {
            machine_start_gcode_end_line_id = line_id;
            VORTEK_LOG(info, "run_pre_scan: detected machine_start_gcode_end_line_id = " << machine_start_gcode_end_line_id);
        }

        // Detect start of end G-code (strictly for machine: [Model] end, ignoring config preset lines)
        if (raw_line.find("machine:") != std::string::npos) {
            std::regex re_end(R"(machine:\s+\w+\s+end)");
            bool is_match = std::regex_search(raw_line, re_end);
            bool has_equals = (raw_line.find(" = ") != std::string::npos);
            
            // Clean log line from newlines for better logs
            std::string log_line = raw_line;
            if (!log_line.empty() && log_line.back() == '\n') log_line.pop_back();
            if (!log_line.empty() && log_line.back() == '\r') log_line.pop_back();

            VORTEK_LOG(warning, "run_pre_scan: evaluating line=" << line_id 
                              << " text=\"" << log_line << "\""
                              << " | matches_regex=" << is_match 
                              << " | has_equals=" << has_equals
                              << " | start_gcode_ended=" << (machine_start_gcode_end_line_id > 0));

            if (machine_start_gcode_end_line_id > 0 && !has_equals && is_match) {
                machine_end_gcode_start_line_id = line_id;
                VORTEK_LOG(warning, "run_pre_scan: SUCCESS detected machine_end_gcode_start_line_id = " << machine_end_gcode_start_line_id);
            }
        }


        if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, "T")) {
            VORTEK_LOG(warning, "run_pre_scan: raw T line = " << raw_line);
            int fid = -1;
            const char* p_space = raw_line.data();
            while (*p_space == ' ' || *p_space == '\t') ++p_space;
            int skips = p_space - raw_line.data();
            std::istringstream str(raw_line.substr(skips + 1));
            str >> fid;
            if (!str.fail() && fid >= 0 && fid < 255) {
                int nozzle_id = -1;
                char param;
                while (str >> param) {
                    if (param == 'H') {
                        str >> nozzle_id;
                        break;
                    }
                }
                handle_filament_change(fid, line_id, nozzle_id);
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";VT")) {
            int fid = -1;
            const char* p_space = raw_line.data();
            while (*p_space == ' ' || *p_space == '\t') ++p_space;
            int skips = p_space - raw_line.data();
            std::istringstream str(raw_line.substr(skips + 3));
            str >> fid;
            if (!str.fail() && fid >= 0 && fid < 255) {
                int nozzle_id = -1;
                char param;
                while (str >> param) {
                    if (param == 'H') {
                        str >> nozzle_id;
                        break;
                    }
                }
                handle_filament_change(fid, line_id, nozzle_id);
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, "M1020")) {
            size_t s_pos = raw_line.find('S');
            if (s_pos != std::string::npos) {
                std::istringstream str(raw_line.substr(s_pos + 1));
                int fid = -1;
                str >> fid;
                if (!str.fail() && fid >= 0 && fid < 255) {
                    int nozzle_id = -1;
                    char param;
                    while (str >> param) {
                        if (param == 'H') {
                            str >> nozzle_id;
                            break;
                        }
                    }
                    handle_filament_change(fid, line_id, nozzle_id);
                }
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";_NOZZLE_CHANGE_START")) {
            int prev_filament = -1, next_filament = -1, extruder_id = -1, prev_nozzle_id = -1, next_nozzle_id = -1;
            handle_nozzle_change_line(raw_line, prev_filament, next_filament, extruder_id, prev_nozzle_id, next_nozzle_id);
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().initialize_step_2(line_id);
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";_NOZZLE_CHANGE_END")) {
            int prev_filament = -1, next_filament = -1, extruder_id = -1, prev_nozzle_id = -1, next_nozzle_id = -1;
            handle_nozzle_change_line(raw_line, prev_filament, next_filament, extruder_id, prev_nozzle_id, next_nozzle_id);
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().initialize_step_3(line_id, prev_filament, line_id, prev_nozzle_id);
            }
            ExtruderUsageBlock temp_construct_block;
            temp_construct_block.initialize_step_1(extruder_id, line_id, next_filament, next_nozzle_id);
            extruder_blocks.emplace_back(temp_construct_block);
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";_CP_TOOLCHANGE_WIPE")) {
            std::regex re(R"(CT(\d)(?:\s+FL(\d))?)");
            std::smatch match;
            bool is_contact = false;
            bool is_first_layer = false;
            if (std::regex_search(raw_line, match, re)) {
                is_contact = std::stoi(match[1]);
                is_first_layer = match[2].matched ? std::stoi(match[2]) != 0 : false;
            }
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().ignore_cooling_before_tower = is_contact || is_first_layer;
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";LAYER_CHANGE")) {
            ++current_layer_id;
        }
    });

    if (!filament_blocks.empty()) {
        filament_blocks.back().upper_gcode_id = machine_end_gcode_start_line_id;
    }

    if (!extruder_blocks.empty()) {
        int first_filament = 0;
        int last_filament = 0;
        if (!filament_blocks.empty()) {
            first_filament = filament_blocks.front().filament_id;
            last_filament = filament_blocks.back().filament_id;
        }
        int first_extruder_id = -1;
        auto nozzle_info = nozzle_group.get_first_nozzle_for_filament(first_filament);
        if (nozzle_info)
            first_extruder_id = nozzle_info->extruder_id;
        int start_nozzle_id = nozzle_info ? nozzle_info->group_id : -1;
        extruder_blocks.front().initialize_step_1(first_extruder_id, machine_start_gcode_end_line_id, first_filament, start_nozzle_id);

        extruder_blocks.back().initialize_step_2(machine_end_gcode_start_line_id);
        int last_nozzle_id = -1;
        if (!filament_blocks.empty())
            last_nozzle_id = filament_blocks.back().nozzle_id;
        extruder_blocks.back().initialize_step_3(machine_end_gcode_start_line_id, last_filament, machine_end_gcode_start_line_id, last_nozzle_id);
    }

    // Retrieve values from print configuration
    std::vector<int> filament_nozzle_temps(print_config.nozzle_temperature.values);
    std::vector<int> filament_nozzle_temps_initial_layer(print_config.nozzle_temperature_initial_layer.values);
    std::vector<int> physical_extruder_map(print_config.physical_extruder_map.values);
    
    int standby_temp_delta = print_config.standby_temperature_delta.value;

    std::vector<int> pre_cooling_temp_nc(print_config.filament_pre_cooling_temperature_nc.values.size());
    for (size_t i = 0; i < pre_cooling_temp_nc.size(); ++i) {
        pre_cooling_temp_nc[i] = print_config.filament_pre_cooling_temperature_nc.get_at(i);
    }

    std::vector<int> filament_idle_temps(print_config.idle_temperature.values.size());
    for (size_t i = 0; i < filament_idle_temps.size(); ++i) {
        filament_idle_temps[i] = print_config.idle_temperature.get_at(i);
    }

    std::vector<double> cooling_rate;
    if (print_config.hotend_cooling_rate.values.empty()) {
        cooling_rate.resize(print_config.nozzle_diameter.values.size(), 0.5);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            cooling_rate.push_back(print_config.hotend_cooling_rate.get_at(i));
        }
    }

    std::vector<double> heating_rate;
    if (print_config.hotend_heating_rate.values.empty()) {
        heating_rate.resize(print_config.nozzle_diameter.values.size(), 2.0);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            heating_rate.push_back(print_config.hotend_heating_rate.get_at(i));
        }
    }

    std::vector<int> extruder_max_nozzle_count;
    if (print_config.extruder_max_nozzle_count.values.empty()) {
        extruder_max_nozzle_count.resize(print_config.nozzle_diameter.values.size(), 1);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            extruder_max_nozzle_count.push_back(print_config.extruder_max_nozzle_count.get_at(i));
        }
    }

    // Preheat temperature delta and max temperature drop when EC (Hardcoded defaults)
    std::vector<double> filament_preheat_temperature_delta(print_config.filament_type.values.size(), 50.0);
    std::vector<double> filament_max_temperature_drop_when_ec(print_config.filament_type.values.size(), 50.0);

    std::vector<Slic3r::ExtruderType> extruder_types; // empty
    std::vector<double> nozzle_diameter(print_config.nozzle_diameter.values);

    int valid_machine_id = 0;
    for (size_t i = 0; i < static_cast<size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (processor.m_time_processor.machines[i].enabled) {
            valid_machine_id = i;
            break;
        }
    }

    PreCooling pre_cooling_processor(
        processor.m_result.moves,
        nozzle_group,
        filament_nozzle_temps,
        filament_nozzle_temps_initial_layer,
        physical_extruder_map,
        valid_machine_id,
        0.0f, // inject_time_threshold
        false, // handle_hotend_as_extruder
        print_config.has_filament_switcher.value,
        standby_temp_delta,
        pre_cooling_temp_nc,
        filament_idle_temps,
        cooling_rate,
        heating_rate,
        skippable_blocks,
        extruder_max_nozzle_count,
        filament_preheat_temperature_delta,
        filament_max_temperature_drop_when_ec,
        machine_start_gcode_end_line_id,
        machine_end_gcode_start_line_id,
        extruder_types,
        nozzle_diameter
    );

    pre_cooling_processor.build_extruder_free_blocks(filament_blocks, extruder_blocks);
    pre_cooling_processor.process_pre_cooling_and_heating(inserted_operation_lines);

    return inserted_operation_lines;
}

void PreCooling::inject_lines(
    InsertedLinesMap::iterator& precooling_iter,
    const InsertedLinesMap& precooling_inserted_lines,
    bool enable_pre_heating,
    unsigned int line_id,
    std::function<void(const std::string&)> append_line_fn
)
{
    if (precooling_iter != precooling_inserted_lines.end() && line_id == precooling_iter->first) {
        VORTEK_LOG(debug, "injecting planned pre-cooling/heating lines at line_id " << line_id << ", lines count = " << precooling_iter->second.size());
        for (const auto& elem : precooling_iter->second) {
            if (enable_pre_heating) {
                append_line_fn(elem.first);
            }
        }
        ++precooling_iter;
    }
}

} // namespace Vortek
