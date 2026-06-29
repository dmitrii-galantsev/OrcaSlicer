#include "VortekGCode.hpp"
#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/VortekLog.hpp"
#include <algorithm>

namespace Vortek {
namespace GCodeHooks {

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

    // Register filament_pre_cooling_temperature_nc in placeholder_parser
    gcode.placeholder_parser().set("filament_pre_cooling_temperature_nc", new Slic3r::ConfigOptionIntsNullable(gcode.m_config.filament_pre_cooling_temperature_nc));
}

void patch_toolchange_dyn_config(
    Slic3r::GCode& gcode,
    Slic3r::DynamicConfig& dyn_config,
    int new_filament_id,
    int layer_id)
{
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
}

} // namespace GCodeHooks
} // namespace Vortek
