#include "VortekGCode.hpp"
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
    // Initialize Vortek state in the parser itself (no statics!).
    // These are read and updated by patch_toolchange_dyn_config().
    parser.set("vortek_real_toolchange_count", 0);
    parser.set("vortek_extruders_used_mask", 0);

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
    // Register all H2C/BBL placeholders and NC variables
    register_vortek_placeholders(gcode.placeholder_parser(), gcode.m_config, gcode.m_print);

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
    // ── FIX: Override toolchange_count for H2C firmware ──────────────────
    // OrcaSlicer's m_toolchange_count includes ALL internal extruder switches
    // (including virtual WipeTower operations), producing values like 29, 72, 142...
    // H2C firmware expects sequential numbering: 1, 2, 3, 4...
    // The template uses: M620 O{toolchange_count + 1}
    // Counter state lives in the PlaceholderParser — no statics, no globals.
    int real_tc = 0;
    if (auto* opt = gcode.placeholder_parser().option("vortek_real_toolchange_count")) {
        if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
            real_tc = opt_int->value;
    }
    real_tc++;
    gcode.placeholder_parser().set("vortek_real_toolchange_count", real_tc);
    dyn_config.set_key_value("toolchange_count", new Slic3r::ConfigOptionInt(real_tc));
    Slic3r::Print* print = gcode.m_print;
    if (!print) return;

    auto group_result = print->get_layered_nozzle_group_result();
    if (!group_result) return;

    // Retrieve nozzle info for the incoming logical filament
    auto nozzle_info = group_result->get_nozzle_for_filament(new_filament_id, layer_id);
    if (!nozzle_info.has_value()) return;

    int extruder_id = nozzle_info->extruder_id;
    float diameter = std::stof(nozzle_info->diameter);

    VORTEK_LOG(info, "patching toolchange config for filament " << new_filament_id 
                    << " on layer " << layer_id << " (physical extruder: " << extruder_id 
                    << ", nozzle diameter: " << diameter << ")");

    // 1. Dynamic Override retraction values based on active nozzle slot settings
    if (new_filament_id < (int)gcode.m_config.filament_retract_length_nc.values.size()) {
        float nc_len = gcode.m_config.filament_retract_length_nc.values[new_filament_id];
        if (nc_len > 0.0f) {
            gcode.m_config.retraction_length.values[new_filament_id] = nc_len;
            gcode.m_writer.config.retraction_length.values[new_filament_id] = nc_len;
            dyn_config.set_key_value("retraction_length", new Slic3r::ConfigOptionFloats({(double)nc_len}));
            VORTEK_LOG(debug, "patched retraction_length -> " << nc_len);
        }
    }

    if (new_filament_id < (int)gcode.m_config.filament_retract_lift_nc.values.size()) {
        float nc_lift = gcode.m_config.filament_retract_lift_nc.values[new_filament_id];
        if (nc_lift > 0.0f) {
            gcode.m_config.z_hop.values[new_filament_id] = nc_lift;
            gcode.m_writer.config.z_hop.values[new_filament_id] = nc_lift;
            dyn_config.set_key_value("z_hop", new Slic3r::ConfigOptionFloats({(double)nc_lift}));
            VORTEK_LOG(debug, "patched z_hop -> " << nc_lift);
        }
    }

    if (new_filament_id < (int)gcode.m_config.filament_retract_speed_nc.values.size()) {
        int nc_speed = gcode.m_config.filament_retract_speed_nc.values[new_filament_id];
        if (nc_speed > 0) {
            gcode.m_config.retraction_speed.values[new_filament_id] = nc_speed;
            gcode.m_writer.config.retraction_speed.values[new_filament_id] = nc_speed;
            dyn_config.set_key_value("retraction_speed", new Slic3r::ConfigOptionFloats({(double)nc_speed}));
            VORTEK_LOG(debug, "patched retraction_speed -> " << nc_speed);
        }
    }

    if (new_filament_id < (int)gcode.m_config.filament_deretract_speed_nc.values.size()) {
        int nc_deretract_speed = gcode.m_config.filament_deretract_speed_nc.values[new_filament_id];
        if (nc_deretract_speed > 0) {
            gcode.m_config.deretraction_speed.values[new_filament_id] = nc_deretract_speed;
            gcode.m_writer.config.deretraction_speed.values[new_filament_id] = nc_deretract_speed;
            dyn_config.set_key_value("deretraction_speed", new Slic3r::ConfigOptionFloats({(double)nc_deretract_speed}));
            VORTEK_LOG(debug, "patched deretraction_speed -> " << nc_deretract_speed);
        }
    }

    // 2. Override nozzle diameter for the target physical extruder
    if (extruder_id < (int)gcode.m_config.nozzle_diameter.values.size()) {
        gcode.m_config.nozzle_diameter.values[extruder_id] = diameter;
        dyn_config.set_key_value("nozzle_diameter", new Slic3r::ConfigOptionFloats(gcode.m_config.nozzle_diameter.values));
        VORTEK_LOG(debug, "patched nozzle_diameter for extruder " << extruder_id << " -> " << diameter);
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

        auto& parser = gcode.placeholder_parser();
        parser.set("long_retraction_when_cut", get_vec_bool("long_retractions_when_cut", current_extruder, false));
        parser.set("retraction_distance_when_cut", get_vec_float("retraction_distances_when_cut", current_extruder, 0.0));
        parser.set("long_retraction_when_ec", get_vec_bool("long_retractions_when_ec", current_extruder, false));
        parser.set("retraction_distance_when_ec", get_vec_float("retraction_distances_when_ec", current_extruder, 0.0));

        // filament_retract_length_nc is evaluated as a scalar of the incoming filament in BBL template!
        parser.set("filament_retract_length_nc", get_vec_float("filament_retract_length_nc", next_extruder, 0.0));

        std::string old_variant = "Direct Drive Standard";
        std::string new_variant = "Direct Drive Standard";
        if (gcode.m_config.has("filament_extruder_variant")) {
            auto opt = gcode.m_config.option<Slic3r::ConfigOptionStrings>("filament_extruder_variant");
            if (opt) {
                if (current_extruder < (int)opt->values.size()) old_variant = opt->values[current_extruder];
                if (next_extruder < (int)opt->values.size()) new_variant = opt->values[next_extruder];
            }
        }
        parser.set("old_extruder_variant", old_variant);
        parser.set("new_extruder_variant", new_variant);

        // new_extruder_retracted_length reflects the actual retract state of the incoming extruder.
        // First use of an extruder: 0 (filament was just loaded, not retracted yet).
        // Subsequent uses: retract_length_toolchange (extruder was retracted during previous toolchange).
        // BBL reference confirms: R0 for first toolchange, R2 for subsequent ones.
        double new_retract = 0.0;
        {
            // Track which extruders have been used via a bitmask in PlaceholderParser.
            int used_mask = 0;
            if (auto* opt = gcode.placeholder_parser().option("vortek_extruders_used_mask")) {
                if (auto* opt_int = dynamic_cast<const Slic3r::ConfigOptionInt*>(opt))
                    used_mask = opt_int->value;
            }
            bool already_used = (used_mask >> next_extruder) & 1;
            if (already_used) {
                if (gcode.m_config.has("retract_length_toolchange")) {
                    auto opt = gcode.m_config.option<Slic3r::ConfigOptionFloats>("retract_length_toolchange");
                    if (opt && next_extruder < (int)opt->values.size())
                        new_retract = opt->values[next_extruder];
                }
            }
            // Mark this extruder as used for future toolchanges.
            used_mask |= (1 << next_extruder);
            gcode.placeholder_parser().set("vortek_extruders_used_mask", used_mask);
        }
        parser.set("new_extruder_retracted_length", new_retract);
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
