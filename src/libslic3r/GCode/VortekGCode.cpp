#include "VortekGCode.hpp"
#include <libslic3r/PlaceholderParser.hpp>
#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/VortekLog.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace Vortek {
namespace GCodeHooks {


void register_vortek_placeholders(
    Slic3r::PlaceholderParser& parser,
    const Slic3r::FullPrintConfig& config,
    const Slic3r::Print* print)
{
    // Register all H2C/BBL placeholders and NC variables if the printer supports H2C parameters.
    // This is done early to ensure they are available even for single-nozzle plates or during early slicing stages.
    // Initialize Vortek state in the parser itself (no statics, no memory leaks!).
    // vortek_extruders_unloaded_mask: bit N=1 means nozzle slot N has been unloaded and is in parking box.
    parser.set("vortek_extruders_unloaded_mask", 0);
    // vortek_toolchange_count: our own counter that matches BBS m_toolchange_count semantics.
    parser.set("vortek_toolchange_count", 0);
    // vortek_last_filament_id: tracks physical switches to increment count only when changing filament IDs.
    parser.set("vortek_last_filament_id", -1);

    if (!config.has("filament_pre_cooling_temperature_nc")) return;

    parser.set("filament_pre_cooling_temperature_nc", new Slic3r::ConfigOptionIntsNullable(config.filament_pre_cooling_temperature_nc));
    parser.set("filament_ramming_volumetric_speed_nc", new Slic3r::ConfigOptionFloatsNullable(config.filament_ramming_volumetric_speed_nc));
    parser.set("filament_ramming_travel_time_nc", new Slic3r::ConfigOptionFloatsNullable(config.filament_ramming_travel_time_nc));
    parser.set("filament_change_length_nc", new Slic3r::ConfigOptionFloats(config.filament_change_length_nc));
    parser.set("filament_prime_volume_nc", new Slic3r::ConfigOptionFloats(config.filament_prime_volume_nc));
    parser.set("filament_retract_length_nc", new Slic3r::ConfigOptionFloats(config.filament_retract_length_nc));
    parser.set("filament_retract_lift_nc", new Slic3r::ConfigOptionFloats(config.filament_retract_lift_nc));
    parser.set("filament_retract_speed_nc", new Slic3r::ConfigOptionInts(config.filament_retract_speed_nc));
    parser.set("filament_deretract_speed_nc", new Slic3r::ConfigOptionInts(config.filament_deretract_speed_nc));

    bool tower_valid = config.enable_prime_tower.value;
    parser.set("wipe_tower_center_pos_valid", tower_valid);
    parser.set("wipe_tower_center_pos_x", tower_valid ? (config.wipe_tower_x.values.empty() ? 95.5 : config.wipe_tower_x.values[0]) : 95.5);
    parser.set("wipe_tower_center_pos_y", tower_valid ? (config.wipe_tower_y.values.empty() ? 336.0 : config.wipe_tower_y.values[0]) : 336.0);

    parser.set("cooling_filter_enabled", false);
    parser.set("old_extruder_variant", std::string("Direct Drive Standard"));
    parser.set("new_extruder_variant", std::string("Direct Drive Standard"));
    parser.set("new_extruder_retracted_length", 0.0);

    std::vector<double> heat_rates = {3.5, 13.3};
    std::vector<double> cool_rates = {1.6, 3.4};
    parser.set("hotend_heating_rate", new Slic3r::ConfigOptionFloats(heat_rates));
    parser.set("hotend_cooling_rate", new Slic3r::ConfigOptionFloats(cool_rates));

    int first_non_support_extruder_id = 0;
    if (print) {
        auto non_support_extruders = print->extruders(false);
        if (!non_support_extruders.empty()) {
            first_non_support_extruder_id = non_support_extruders.front();
        }
    }
    
    auto get_vec_int = [&](const std::string& key, int idx, int def_val) -> int {
        if (config.has(key)) {
            auto opt = config.option<Slic3r::ConfigOptionInts>(key);
            if (opt && idx < (int)opt->values.size()) return opt->values[idx];
            auto opt_null = config.option<Slic3r::ConfigOptionIntsNullable>(key);
            if (opt_null && idx < (int)opt_null->values.size()) return opt_null->values[idx];
        }
        return def_val;
    };

    int first_non_support_hotend_val = get_vec_int("filament_map_2", first_non_support_extruder_id, first_non_support_extruder_id);
    
    std::vector<std::string> first_non_support_filaments_vec = { std::to_string(first_non_support_extruder_id) };
    std::vector<std::string> first_non_support_hotend_vec = { std::to_string(first_non_support_hotend_val) };
    
    parser.set("first_non_support_filaments", first_non_support_filaments_vec);
    parser.set("first_non_support_hotend", first_non_support_hotend_vec);
}

void update_layer_related_config(Slic3r::GCode& gcode, int layer_id)
{
    Slic3r::Print* print = gcode.m_print;
    if (!print) return;

    auto group_result = print->get_layered_nozzle_group_result();
    if (!group_result) return;

    // Retrieve active extruder/volume/nozzle maps for the layer
    auto extruder_map = group_result->get_extruder_map(false, layer_id);
    auto volume_map = group_result->get_volume_map(layer_id);
    auto nozzle_map = group_result->get_nozzle_map(layer_id);

    VORTEK_LOG(debug, "update_layer_related_config on layer " << layer_id);

    // Apply mappings to GCode generator configuration
    gcode.m_config.filament_map.values = extruder_map;
    gcode.m_config.filament_volume_map.values = volume_map;
    gcode.m_config.filament_nozzle_map.values = nozzle_map;

    gcode.m_writer.config.filament_map.values = extruder_map;
    gcode.m_writer.config.filament_volume_map.values = volume_map;
    gcode.m_writer.config.filament_nozzle_map.values = nozzle_map;
}

void patch_toolchange_dyn_config(
    Slic3r::GCode& gcode,
    Slic3r::DynamicConfig& dyn_config,
    int new_filament_id,
    int layer_id)
{
    Slic3r::Print* print = gcode.m_print;
    if (!print) return;

    // ── FIX: Override toolchange_count for H2C firmware ──────────────────
    int config_extruder_idx = new_filament_id % 2; // Default fallback for 0-based arrays
    int extruder_id = new_filament_id;             // Default fallback (carriage)
    int nozzle_id = new_filament_id;               // Default fallback (nozzle)
    float diameter = 0.4f;

    // Try to get nozzle_id from static filament_nozzle_map in config
    if (gcode.m_config.has("filament_nozzle_map")) {
        auto opt = gcode.m_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
        if (opt && new_filament_id >= 0 && new_filament_id < (int)opt->values.size()) {
            nozzle_id = opt->values[new_filament_id];
            
            // Check if map is flat/uninitialized (all 1s)
            bool is_flat_map = true;
            for (int val : opt->values) {
                if (val != 1) { is_flat_map = false; break; }
            }
            if (is_flat_map && gcode.m_config.has("filament_map")) {
                auto f_map_opt = gcode.m_config.option<Slic3r::ConfigOptionInts>("filament_map");
                if (f_map_opt && opt->values.size() == f_map_opt->values.size()) {
                    std::vector<int> calculated_nozzles(opt->values.size(), 0);
                    int next_carousel_nozzle = 3;
                    for (size_t i = 0; i < f_map_opt->values.size(); ++i) {
                        int ext_id = f_map_opt->values[i]; // 1-based (1 = Left, 2 = Right)
                        if (ext_id == 1) {
                            calculated_nozzles[i] = 0; // Left is always 0
                        } else if (ext_id == 2) {
                            calculated_nozzles[i] = next_carousel_nozzle--;
                            if (next_carousel_nozzle < 1) next_carousel_nozzle = 3;
                        }
                    }
                    nozzle_id = calculated_nozzles[new_filament_id];
                }
            }
        }
    }
    // Try to get nozzle diameter from config
    if (gcode.m_config.has("nozzle_diameter") && new_filament_id < (int)gcode.m_config.nozzle_diameter.values.size())
        diameter = gcode.m_config.nozzle_diameter.values[new_filament_id];

    auto group_result = print->get_layered_nozzle_group_result();
    if (group_result) {
        // Retrieve dynamic nozzle info if available
        auto nozzle_info = group_result->get_nozzle_for_filament(new_filament_id, layer_id);
        if (nozzle_info.has_value()) {
            extruder_id = nozzle_info->extruder_id;
            config_extruder_idx = extruder_id - 1; // Convert 1-based physical ID to 0-based array index
            nozzle_id = nozzle_info->group_id; // Physical nozzle changer slot ID
            diameter = std::stof(nozzle_info->diameter);
        }
    }

    // ── toolchange_count (M620 O{...}) ───────────────────────────────────────
    // BambuStudio: m_toolchange_count is incremented ONLY when actually changing filaments,
    //   while OrcaSlicer's native counter gets inflated by wipe tower segments.
    //
    // Fix: Track the last processed filament ID. Increment the counter ONLY when
    //   new_filament_id differs from the last successfully processed filament.
    //   The first tool load (transition from last_filament_id=-1) updates the state
    //   but does not increment the counter, keeping the start count at 0 (gives M620 O1).
    int last_filament_id = -1;
    if (auto* opt = gcode.placeholder_parser().option("vortek_last_filament_id")) {
        if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
            last_filament_id = opt_int->value;
    }

    int tc = 0;
    if (auto* opt = gcode.placeholder_parser().option("vortek_toolchange_count")) {
        if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
            tc = opt_int->value;
    }

    if (new_filament_id != last_filament_id) {
        if (last_filament_id != -1) {
            tc++; // Physical switch transition occurred
        }
        gcode.placeholder_parser().set("vortek_last_filament_id", new_filament_id);
        gcode.placeholder_parser().set("vortek_toolchange_count", tc);
        VORTEK_LOG(info, "toolchange_count transition: " << last_filament_id << " -> " << new_filament_id << ", tc=" << tc);
    }

    dyn_config.set_key_value("toolchange_count", new Slic3r::ConfigOptionInt(tc));

    VORTEK_LOG(warning, "patching toolchange config for filament " << new_filament_id
                        << " (extruder " << extruder_id << ", nozzle diameter " << diameter
                        << ", tc=" << tc << ")");

    // 2. Override nozzle diameter for the target physical extruder
    if (config_extruder_idx >= 0 && config_extruder_idx < (int)gcode.m_config.nozzle_diameter.values.size()) {
        gcode.m_config.nozzle_diameter.values[config_extruder_idx] = diameter;
        dyn_config.set_key_value("nozzle_diameter", new Slic3r::ConfigOptionFloats(gcode.m_config.nozzle_diameter.values));
        VORTEK_LOG(debug, "patched nozzle_diameter for extruder " << config_extruder_idx << " -> " << diameter);
    }

    // 3. Dynamic Override filament_pre_cooling_temperature_nc for toolchange
    if (!gcode.m_config.filament_pre_cooling_temperature_nc.values.empty()) {
        auto filament_pre_cooling_temperature_nc = gcode.m_config.filament_pre_cooling_temperature_nc.values;
        if (filament_pre_cooling_temperature_nc.size() < gcode.m_config.filament_type.values.size())
            filament_pre_cooling_temperature_nc.resize(gcode.m_config.filament_type.values.size(), gcode.m_config.filament_pre_cooling_temperature_nc.get_at(0));
        dyn_config.set_key_value("filament_pre_cooling_temperature_nc", new Slic3r::ConfigOptionInts(filament_pre_cooling_temperature_nc));
        VORTEK_LOG(debug, "patched filament_pre_cooling_temperature_nc");
    }

    // 4. Vortek (H2C) dynamic compatibility mapping for the current toolchange
    if (gcode.m_config.has("filament_pre_cooling_temperature_nc")) {
        int current_extruder = gcode.writer().filament() ? gcode.writer().filament()->id() : 0;
        int next_extruder = new_filament_id;

        auto get_vec_bool = [&](const std::string& key, int idx, bool def_val) -> bool {
            if (gcode.m_config.has(key)) {
                auto opt = gcode.m_config.option<Slic3r::ConfigOptionBools>(key);
                if (opt && idx < (int)opt->values.size()) return opt->values[idx];
                // Also handle nullable bools (e.g. long_retractions_when_ec)
                auto opt_null = gcode.m_config.option<Slic3r::ConfigOptionBoolsNullable>(key);
                if (opt_null && idx < (int)opt_null->values.size()) return opt_null->values[idx];
            }
            return def_val;
        };
        auto get_vec_float = [&](const std::string& key, int idx, double def_val) -> double {
            if (gcode.m_config.has(key)) {
                auto opt = gcode.m_config.option<Slic3r::ConfigOptionFloats>(key);
                if (opt && idx < (int)opt->values.size()) return opt->values[idx];
                auto opt_null = gcode.m_config.option<Slic3r::ConfigOptionFloatsNullable>(key);
                if (opt_null && idx < (int)opt_null->values.size()) return opt_null->values[idx];
            }
            return def_val;
        };

        dyn_config.set_key_value("long_retraction_when_cut", new Slic3r::ConfigOptionBool(get_vec_bool("long_retractions_when_cut", current_extruder, false)));
        dyn_config.set_key_value("retraction_distance_when_cut", new Slic3r::ConfigOptionFloat(get_vec_float("retraction_distances_when_cut", current_extruder, 0.0)));
        dyn_config.set_key_value("long_retraction_when_ec", new Slic3r::ConfigOptionBool(get_vec_bool("long_retractions_when_ec", current_extruder, false)));
        dyn_config.set_key_value("retraction_distance_when_ec", new Slic3r::ConfigOptionFloat(get_vec_float("retraction_distances_when_ec", current_extruder, 0.0)));

        // filament_retract_length_nc is evaluated as a scalar of the incoming filament in BBL template!
        dyn_config.set_key_value("filament_retract_length_nc", new Slic3r::ConfigOptionFloat(get_vec_float("filament_retract_length_nc", next_extruder, 0.0)));

        std::string old_variant = "Direct Drive Standard";
        std::string new_variant = "Direct Drive Standard";
        if (gcode.m_config.has("filament_extruder_variant")) {
            auto opt = gcode.m_config.option<Slic3r::ConfigOptionStrings>("filament_extruder_variant");
            if (opt) {
                if (current_extruder < (int)opt->values.size()) old_variant = opt->values[current_extruder];
                if (next_extruder < (int)opt->values.size()) new_variant = opt->values[next_extruder];
            }
        }
        dyn_config.set_key_value("old_extruder_variant", new Slic3r::ConfigOptionString(old_variant));
        dyn_config.set_key_value("new_extruder_variant", new Slic3r::ConfigOptionString(new_variant));

        // new_extruder_retracted_length reflects the actual retract state of the incoming extruder.
        double new_retract = 0.0;
        {
            // Track which nozzles have been unloaded and parked in the carousel box.
            int unloaded_mask = 0;
            if (auto* opt = gcode.placeholder_parser().option("vortek_extruders_unloaded_mask")) {
                if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
                    unloaded_mask = opt_int->value;
            }

            // Identify initial (start) nozzle ID
            int initial_tool = 0;
            if (auto* opt = gcode.placeholder_parser().option("initial_tool")) {
                if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
                    initial_tool = opt_int->value;
            }
            int initial_nozzle_id = initial_tool;
            if (gcode.m_config.has("filament_nozzle_map")) {
                auto opt = gcode.m_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
                if (opt && initial_tool >= 0 && initial_tool < (int)opt->values.size()) {
                    initial_nozzle_id = opt->values[initial_tool];
                }
            }
            auto group_result = print->get_layered_nozzle_group_result();
            if (group_result) {
                auto nozzle_info = group_result->get_nozzle_for_filament(initial_tool, 0);
                if (nozzle_info.has_value()) {
                    initial_nozzle_id = nozzle_info->group_id;
                }
            }

            // If there was a previous tool active, mark its nozzle as unloaded (parked in carousel)
            if (last_filament_id != -1) {
                int old_nozzle_id = last_filament_id;
                if (gcode.m_config.has("filament_nozzle_map")) {
                    auto opt = gcode.m_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
                    if (opt && last_filament_id >= 0 && last_filament_id < (int)opt->values.size()) {
                        old_nozzle_id = opt->values[last_filament_id];
                    }
                }
                if (group_result) {
                    auto nozzle_info = group_result->get_nozzle_for_filament(last_filament_id, layer_id);
                    if (nozzle_info.has_value()) {
                        old_nozzle_id = nozzle_info->group_id;
                    }
                }
                if (old_nozzle_id != nozzle_id) {
                    unloaded_mask |= (1 << old_nozzle_id);
                    gcode.placeholder_parser().set("vortek_extruders_unloaded_mask", unloaded_mask);
                    VORTEK_LOG(info, "nozzle parked: old_nozzle_id=" << old_nozzle_id << ", unloaded_mask=" << unloaded_mask);
                }
            }

            // Determine retracted state for the target nozzle_id
            bool already_unloaded = (unloaded_mask >> nozzle_id) & 1;
            bool is_parked_type = (nozzle_id > 0); // Nozzle 0 is static, nozzle 1,2,3 are in carousel

            bool requires_unretract = false;
            if (is_parked_type) {
                if (already_unloaded) {
                    requires_unretract = true;
                } else {
                    // First load check: is it the start nozzle or carousel parked nozzle?
                    if (nozzle_id != initial_nozzle_id) {
                        requires_unretract = true; // Carousel nozzle starts in the parked state
                    }
                }
            }

            if (requires_unretract) {
                if (gcode.m_config.has("retract_length_toolchange")) {
                    auto opt = gcode.m_config.option<Slic3r::ConfigOptionFloats>("retract_length_toolchange");
                    if (opt && config_extruder_idx >= 0 && config_extruder_idx < (int)opt->values.size()) {
                        new_retract = opt->values[config_extruder_idx];
                    }
                }
            }

            VORTEK_LOG(info, "new_extruder_retracted_length: nozzle_id=" << nozzle_id 
                             << ", initial_nozzle_id=" << initial_nozzle_id
                             << ", already_unloaded=" << already_unloaded 
                             << ", requires_unretract=" << requires_unretract
                             << ", new_retract=" << new_retract);
        }
        dyn_config.set_key_value("new_extruder_retracted_length", new Slic3r::ConfigOptionFloat(new_retract));
    }
}

/**
 * @brief Overrides the hotend ID for G-code placeholder calculation.
 * 
 * In the original OrcaSlicer/BambuStudio for H2C printers, physical hotend IDs 
 * (H0/H1) are output by default. However, if the Filament Track Switch (FTS) 
 * hardware is absent or disabled, the printer firmware expects legacy H-1 and B-1.
 * Emitting H0/H1 on a machine without FTS will trigger firmware error [0700-8029].
 *
 * This hook divides behavior based on `has_filament_switcher`:
 * - has_filament_switcher = false (Classic mode / No-FTS):
 *   Returns -1, which forces the G-code placeholders to evaluate to H-1 and B-1.
 *   The printer accepts this job and prints without requiring FTS.
 * - has_filament_switcher = true (FTS mode):
 *   Returns the original hotend_id (0 or 1), enabling FTS-based A/B (MAIN/DEPUTY) channel switching.
 */
int hotend_id_override(const Slic3r::FullPrintConfig& config, int hotend_id)
{
    // ── NO-FTS BRANCH (default path) ─────────────────────────────────────────
    // `has_filament_switcher` defaults to false.
    // If the FTS module is not installed, force returning -1
    // so OrcaSlicer emits H-1 / B-1 in the generated G-code.
    if (!config.has_filament_switcher.value) {
        return -1;
    }

    // ── FTS BRANCH ──────────────────────────────────────────────────────────
    // Executed only when `has_filament_switcher` is explicitly true.
    // Maps hotend_id 0 to physical channel A (MAIN) and hotend_id 1 to channel B (DEPUTY).
    // Place any FTS-specific validation or override logic here.
    return hotend_id;
}

} // namespace GCodeHooks
} // namespace Vortek
