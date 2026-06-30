#ifndef VORTEK_PRE_COOLING_HPP
#define VORTEK_PRE_COOLING_HPP

#include <vector>
#include <string>
#include <map>
#include <functional>
#include "GCode/GCodeProcessor.hpp"
#include "VortekMultiNozzle.hpp"

namespace Slic3r {
    class GCodeProcessor;
    class Print;
}

namespace Vortek {

/**
 * @brief Standby temperature planner that scans generated G-code and schedules pre-heating / cooling commands.
 * 
 * This ensures that a nozzle is heated up in time before it arrives from the changer slot,
 * and cooled down when sitting idle, preventing oozing and maximizing print quality.
 */
class PreCooling {
public:
    using InsertedLinesMap = std::map<unsigned int, std::vector<std::pair<std::string, int>>>;

    /**
     * @brief Represents a time window when a physical extruder/nozzle carriage is idle.
     */
    struct ExtruderFreeBlock {
        unsigned int free_lower_gcode_id;   ///< Lower bound of G-code line index
        unsigned int free_upper_gcode_id;   ///< Upper bound of G-code line index
        unsigned int partial_free_lower_id;
        unsigned int partial_free_upper_id;
        int last_filament_id;
        int next_filament_id;
        int last_nozzle_id;
        int next_nozzle_id;
        int extruder_id;                    ///< Target physical extruder carriage
        bool ignore_cooling_before_tower = false;
    };

    /**
     * @brief Represents a time window when a specific logical filament is actively extruding.
     */
    struct FilamentUsageBlock {
        int filament_id;
        int extruder_id;
        int nozzle_id;
        unsigned int lower_gcode_id;
        unsigned int upper_gcode_id;
        FilamentUsageBlock(int filament_id_, int extruder_id_, int nozzle_id_, unsigned int lower_gcode_id_, unsigned int upper_gcode_id_)
            : filament_id(filament_id_), extruder_id(extruder_id_), nozzle_id(nozzle_id_), lower_gcode_id(lower_gcode_id_), upper_gcode_id(upper_gcode_id_) {}
    };

    /**
     * @brief Represents a physical extruder's overall printing timeline block.
     */
    struct ExtruderUsageBlock {
        int extruder_id = -1;
        unsigned int start_id = -1;
        unsigned int end_id = -1;
        int start_filament = -1;
        int end_filament = -1;
        int start_nozzle_id = -1;
        int end_nozzle_id = -1;
        unsigned int post_extrusion_start_id = -1;
        unsigned int post_extrusion_end_id = -1;
        bool ignore_cooling_before_tower = false;

        void initialize_step_1(int extruder_id_, int start_id_, int start_filament_, int start_nozzle_id_) {
            extruder_id = extruder_id_;
            start_id = start_id_;
            start_filament = start_filament_;
            start_nozzle_id = start_nozzle_id_;
        }
        void initialize_step_2(int post_extrusion_start_id_) {
            post_extrusion_start_id = post_extrusion_start_id_;
        }
        void initialize_step_3(int end_id_, int end_filament_, int post_extrusion_end_id_, int end_nozzle_id_) {
            end_id = end_id_;
            end_filament = end_filament_;
            post_extrusion_end_id = post_extrusion_end_id_;
            end_nozzle_id = end_nozzle_id_;
        }
        void reset() {
            *this = ExtruderUsageBlock();
        }
    };

    /**
     * @brief Constructs the temperature planner with slicing metadata and configuration parameters.
     */
    PreCooling(
        const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves,
        const std::vector<std::string>& filament_types,
        const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result,
        const std::vector<int>& filament_nozzle_temps,
        const std::vector<int>& filament_nozzle_temps_initial_layer,
        const std::vector<int>& physical_extruder_map,
        int valid_machine_id,
        float inject_time_threshold,
        bool handle_hotend_as_extruder,
        bool has_filament_switcher,
        const std::vector<int>& pre_cooling_temp,
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
    );

    /**
     * @brief Computes pre-heating and cooling schedule and fills the inserted operations map.
     */
    void process_pre_cooling_and_heating(InsertedLinesMap& inserted_operation_lines);
    
    /**
     * @brief Resolves idle time blocks between usages.
     */
    void build_extruder_free_blocks(
        const std::vector<FilamentUsageBlock>& filament_usage_blocks,
        const std::vector<ExtruderUsageBlock>& extruder_usage_blocks
    );

    /**
     * @brief Runs a pre-scan over the generated G-code file and populates planned operations.
     */
    static InsertedLinesMap run_pre_scan(Slic3r::GCodeProcessor& processor, const std::string& filename);
    
    /**
     * @brief Injects the planned operations lines into the final G-code stream.
     */
    static void inject_lines(
        InsertedLinesMap::iterator& precooling_iter,
        const InsertedLinesMap& precooling_inserted_lines,
        bool enable_pre_heating,
        unsigned int line_id,
        std::function<void(const std::string&)> append_line_fn
    );

private:
    void inject_cooling_heating_command(
        InsertedLinesMap& inserted_operation_lines,
        const ExtruderFreeBlock& block,
        float curr_temp,
        float target_temp,
        bool pre_cooling,
        bool pre_heating
    );
    
    void build_by_filament_blocks(const std::vector<FilamentUsageBlock>& filament_usage_blocks);
    void build_by_extruder_blocks(const std::vector<ExtruderUsageBlock>& extruder_usage_blocks);

    std::vector<ExtruderFreeBlock> m_extruder_free_blocks;
    std::vector<Slic3r::GCodeProcessorResult::MoveVertex> m_moves;
    const std::vector<std::string>& m_filament_types;
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& m_nozzle_group_result;
    std::vector<int> m_filament_nozzle_temps;
    std::vector<int> m_filament_nozzle_temps_initial_layer;
    std::vector<int> m_physical_extruder_map;
    int m_valid_machine_id;
    float m_inject_time_threshold;
    bool m_handle_hotend_as_extruder;
    bool m_has_filament_switcher;
    std::vector<int> m_filament_pre_cooling_temps;
    std::vector<double> m_cooling_rate;
    std::vector<double> m_heating_rate;
    std::vector<std::pair<unsigned int, unsigned int>> m_skippable_blocks;
    std::vector<int> m_extruder_max_nozzle_count;
    std::vector<double> m_filament_preheat_temperature_delta;
    std::vector<double> m_filament_max_temperature_drop_when_ec;
    unsigned int m_machine_start_gcode_end_id;
    unsigned int m_machine_end_gcode_start_id;
    std::vector<Slic3r::ExtruderType> m_extruder_types;
    std::vector<double> m_nozzle_diameter;
};

} // namespace Vortek

#endif // VORTEK_PRE_COOLING_HPP
