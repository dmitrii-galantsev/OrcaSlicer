#include "VortekPlateMapping.hpp"
#include "VortekLog.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <algorithm>

namespace Vortek {

bool PlateMapping::is_h2c_multi_nozzle(const Slic3r::Print* print)
{
    if (!print) return false;
    if (print->config().has("extruder_max_nozzle_count")) {
        const auto& counts = print->config().option<Slic3r::ConfigOptionInts>("extruder_max_nozzle_count")->values;
        if (std::any_of(counts.begin(), counts.end(), [](int c) { return c > 1; })) {
            return true;
        }
    }
    return false;
}

void PlateMapping::sync_after_slicing(
    Slic3r::DynamicPrintConfig& plate_config,
    Slic3r::FilamentMapMode filament_map_mode,
    const Slic3r::Print* print,
    Slic3r::PresetBundle& preset_bundle
)
{
    if (!print) return;
    auto group_result = print->get_layered_nozzle_group_result();
    if (!group_result) return;

    if (filament_map_mode != Slic3r::fmmManual && filament_map_mode != Slic3r::fmmNozzleManual) {
        VORTEK_LOG(info, "sync_after_slicing: auto map mode active, skipping write to plate config to prevent loop");
        return;
    }

    VORTEK_LOG(info, "sync_after_slicing: updating nozzle maps in plate config");

    auto nozzle_map = group_result->get_nozzle_map(-1);
    auto volume_map = group_result->get_volume_map(-1);

    bool changed = false;
    auto* opt_nozzle = plate_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
    if (!opt_nozzle || opt_nozzle->values != nozzle_map) {
        plate_config.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts(nozzle_map));
        changed = true;
    }
    auto* opt_volume = plate_config.option<Slic3r::ConfigOptionInts>("filament_volume_map");
    if (!opt_volume || opt_volume->values != volume_map) {
        plate_config.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts(volume_map));
        changed = true;
    }

    if (changed) {
        VORTEK_LOG(info, "sync_after_slicing: nozzle maps changed, updated plate config");
    } else {
        VORTEK_LOG(info, "sync_after_slicing: nozzle maps unchanged, skipped updating plate config");
    }
}

void PlateMapping::handle_filament_count_changed(Slic3r::DynamicPrintConfig* config, int filament_count)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_count_changed: resizing maps to " << filament_count);
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.resize(filament_count, 1);
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.resize(filament_count, 1);
    }
}

void PlateMapping::handle_filament_added(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_added: appending default mapping values");
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.push_back(1);
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.push_back(1);
    }
}

void PlateMapping::handle_filament_deleted(Slic3r::DynamicPrintConfig* config, int filament_id)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_deleted: erasing mapping at index " << filament_id);
    if (config->has("filament_nozzle_map")) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
    if (config->has("filament_volume_map")) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
}

void PlateMapping::clear_mappings(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "clear_mappings: clearing all nozzle/volume mappings");
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.clear();
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.clear();
    }
}

LoadMappingResult PlateMapping::load_from_3mf_structure(
    const Slic3r::PlateData* plate_data,
    int filament_count,
    Slic3r::GCodeProcessorResult* gcode_result
)
{
    LoadMappingResult res;
    if (!plate_data) return res;

    VORTEK_LOG(info, "load_from_3mf_structure: loading nozzle mappings");

    if (plate_data->config.has("filament_nozzle_map")) {
        res.filament_nozzle_map = plate_data->config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    }
    if (plate_data->config.has("filament_volume_map")) {
        res.filament_volume_map = plate_data->config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    }

    if (res.filament_nozzle_map.size() != filament_count) {
        res.filament_nozzle_map.resize(filament_count, 1);
    }
    if (res.filament_volume_map.size() != filament_count) {
        res.filament_volume_map.resize(filament_count, 1);
    }
    return res;
}

void PlateMapping::sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count)
{
    VORTEK_LOG(info, "sync_project_config_on_load: verifying loaded map sizes");
    
    // Сброс MQTT-зависимых флагов, которые должны приходить с принтера, а не считываться из 3MF
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>("has_filament_switcher"))
        p->value = false;
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>("enable_filament_dynamic_map"))
        p->value = false;

    // Синхронизация filament_nozzle_map (дефолт 0 — первое сопло)
    auto* nozzle_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_nozzle_map", true);
    if ((int)nozzle_map->values.size() != filament_count) {
        nozzle_map->values.resize(filament_count, 0);
    }

    // Синхронизация filament_volume_map (дефолт 1 — стандартный объем / nvtStandard)
    auto* volume_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_volume_map", true);
    if ((int)volume_map->values.size() != filament_count) {
        volume_map->values.resize(filament_count, 1);
    }
}

void PlateMapping::patch_export_config(Slic3r::DynamicPrintConfig& cfg)
{
    if (!cfg.has("filament_nozzle_map")) {
        cfg.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts({0}));
    }
    if (!cfg.has("filament_volume_map")) {
        cfg.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts({1}));
    }
}

void PlateMapping::patch_slice_filament_nozzle_groups(
    Slic3r::PlateData* plate_data,
    const std::vector<int>& filament_nozzle_map
)
{
    // Called AFTER parse_filament_info() which populates slice_filaments_info but
    // leaves group_id empty. Without group_id, bbs_3mf.cpp falls back to
    // f_maps[i]-1 (0-based extruder index) — wrong for H2C carousel.
    //
    // BBS's FilamentInfo::group_id = nozzle slot ID (0=Left, 1-3=Right carousel)
    // This makes bbs_3mf.cpp write correct <filament group_id="N"> and
    // generates correct 4-nozzle <nozzle> list in slice_info.config.
    if (!plate_data || filament_nozzle_map.empty()) return;

    int patched = 0;
    for (auto& fi : plate_data->slice_filaments_info) {
        if (fi.id >= 0 && fi.id < (int)filament_nozzle_map.size()) {
            fi.group_id = {filament_nozzle_map[fi.id]};
            ++patched;
        }
    }
    VORTEK_LOG(info, "patch_slice_filament_nozzle_groups: patched " << patched
                     << " filaments with nozzle group_ids from filament_nozzle_map");
}

void PlateMapping::patch_plate_data_for_export(
    Slic3r::PlateData* plate_data,
    const std::vector<int>& filament_nozzle_map,
    const std::vector<int>& filament_volume_map,
    const std::vector<int>& filament_maps,
    const Slic3r::DynamicPrintConfig& config,
    const Slic3r::Print* print
)
{
    if (!plate_data) return;
    VORTEK_LOG(info, "patch_plate_data_for_export for plate index " << plate_data->plate_index);
    plate_data->config.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts(filament_nozzle_map));
    plate_data->config.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts(filament_volume_map));
}

void PlateMapping::handle_h2c_mapping_apply(
    Slic3r::Print* print,
    Slic3r::DynamicPrintConfig& new_full_config,
    const Slic3r::DynamicPrintConfig& old_full_config
)
{
    if (new_full_config.has("filament_nozzle_map") && old_full_config.has("filament_nozzle_map")) {
        auto new_nozzle = new_full_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        auto old_nozzle = old_full_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        if (new_nozzle != old_nozzle) {
            VORTEK_LOG(info, "handle_h2c_mapping_apply: synchronizing config update");
        }
    }
}

void PlateMapping::handle_h2c_print_diff(
    Slic3r::Print* print,
    Slic3r::PrintConfig& config,
    Slic3r::DynamicPrintConfig& full_print_config,
    const Slic3r::DynamicPrintConfig& new_full_config,
    std::unordered_set<std::string>& print_diff_set
)
{
    VORTEK_LOG(debug, "handle_h2c_print_diff: checking " << print_diff_set.size() << " changed options");
    std::vector<std::string> keys_to_remove;
    for (const auto& key : print_diff_set) {
        // Suppress invalidation for dynamic override parameters
        if (key == "nozzle_diameter" || key == "retraction_length" || key == "z_hop" || key == "retraction_speed" || key == "deretraction_speed") {
            keys_to_remove.push_back(key);
        }
    }
    for (const auto& key : keys_to_remove) {
        print_diff_set.erase(key);
        VORTEK_LOG(debug, "suppressed false invalidation for key: " << key);
    }
}

bool PlateMapping::get_variant_override_serialized(const Slic3r::ConfigBase* config, const std::string& opt_key, std::string& out_serialized)
{
    if (!config || !config->has(opt_key)) return false;
    out_serialized = config->option(opt_key)->serialize();
    return true;
}

bool PlateMapping::get_variant_override_values(const Slic3r::ConfigBase* config, const std::string& opt_key, std::vector<std::string>& out_values)
{
    if (!config || !config->has(opt_key)) return false;
    out_values = { config->option(opt_key)->serialize() };
    return true;
}

bool PlateMapping::are_models_compatible(const std::string& model1, const std::string& model2)
{
    if (model1 == model2) return true;
    if ((model1 == "O1C" && model2 == "O1C2") || (model1 == "O1C2" && model2 == "O1C")) {
        VORTEK_LOG(debug, "are_models_compatible: matched H2C fallback compatibility for model1: " + model1 + " and model2: " + model2);
        return true;
    }
    return false;
}

} // namespace Vortek
