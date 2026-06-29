#include "VortekPrintHooks.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "VortekLog.hpp"
#include <boost/format.hpp>
#include <set>
#include <unordered_map>

namespace Vortek {

template<typename OptType, typename ValueType>
static void trim_option_values(OptType *opt, const std::vector<int> &trim_param_indices)
{
    std::vector<ValueType> new_values;
    new_values.reserve(trim_param_indices.size());

    for (int idx : trim_param_indices) {
        new_values.emplace_back(opt->get_at(idx));
    }

    opt->values = std::move(new_values);
}

static void update_filament_config_values_for_multiple_extruders(
    Slic3r::DynamicPrintConfig &printer_config,
    const std::unordered_map<int, std::vector<Slic3r::ExtruderNozleInfo>> &filament_extruder_nozzle_infos,
    int extruder_count,
    int extruder_nozzle_volume_count,
    std::set<std::string> &key_set,
    std::string id_name,
    std::string variant_name)
{
    std::vector<int> filament_maps  = printer_config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
    size_t           filament_count = filament_maps.size();

    auto opt_extruder_type      = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric *>(printer_config.option("extruder_type"));
    auto opt_nozzle_volume_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric *>(printer_config.option("nozzle_volume_type"));

    auto             opt_filament_volume_maps = dynamic_cast<const Slic3r::ConfigOptionInts *>(printer_config.option("filament_volume_map"));
    std::vector<int> filament_volume_maps;
    if (opt_filament_volume_maps) filament_volume_maps = opt_filament_volume_maps->values;
    auto             opt_ids = id_name.empty() ? nullptr : dynamic_cast<const Slic3r::ConfigOptionInts *>(printer_config.option(id_name));

    std::vector<int> trim_param_indices;
    trim_param_indices.reserve(filament_count * 2);
    for (int f_index = 0; f_index < (int)filament_count; f_index++) {
        Slic3r::ExtruderType extruder_type = (Slic3r::ExtruderType) (opt_extruder_type->get_at(filament_maps[f_index] - 1));
        Slic3r::NozzleVolumeType nozzle_volume_type = (Slic3r::NozzleVolumeType) (opt_nozzle_volume_type->get_at(filament_maps[f_index] - 1));
        auto iter = filament_extruder_nozzle_infos.find(f_index);
        if (iter != filament_extruder_nozzle_infos.end()) {
            std::vector<Slic3r::ExtruderNozleInfo> nozzle_infos = iter->second;
            for (Slic3r::ExtruderNozleInfo nozzle_info : nozzle_infos) {
                extruder_type = nozzle_info.extruder_type;
                nozzle_volume_type = nozzle_info.nozzle_volume_type;
                int param_index = printer_config.get_index_for_extruder(f_index + 1, id_name, extruder_type, nozzle_volume_type, variant_name);
                if (param_index < 0) {
                    param_index = 0;
                    if (opt_ids) {
                        for (int i = 0; i < (int)opt_ids->values.size(); i++) {
                            if (opt_ids->values[i] == (f_index + 1)) {
                                param_index = i;
                                break;
                            }
                        }
                    }
                }
                trim_param_indices.push_back(param_index);
            }
        } else {
            // filament not used in slicing
            if ((extruder_nozzle_volume_count > extruder_count) && (!filament_volume_maps.empty())) {
                nozzle_volume_type = (Slic3r::NozzleVolumeType) (filament_volume_maps[f_index]);
            }
            int param_index = printer_config.get_index_for_extruder(f_index + 1, id_name, extruder_type, nozzle_volume_type, variant_name);
            if (param_index < 0) {
                param_index = 0;
                if (opt_ids) {
                    for (int i = 0; i < (int)opt_ids->values.size(); i++) {
                        if (opt_ids->values[i] == (f_index + 1)) {
                            param_index = i;
                            break;
                        }
                    }
                }
            }
            trim_param_indices.push_back(param_index);
        }
    }

    const Slic3r::ConfigDef *config_def = printer_config.def();
    if (!config_def) return;
    for (auto &key : key_set) {
        const Slic3r::ConfigOptionDef *optdef = config_def->get(key);
        if (!optdef) continue;
        switch (optdef->type) {
        case Slic3r::coStrings: {
            trim_option_values<Slic3r::ConfigOptionStrings, std::string>(printer_config.option<Slic3r::ConfigOptionStrings>(key), trim_param_indices);
            break;
        }
        case Slic3r::coInts: {
            trim_option_values<Slic3r::ConfigOptionInts, int>(printer_config.option<Slic3r::ConfigOptionInts>(key), trim_param_indices);
            break;
        }
        case Slic3r::coFloats: {
            trim_option_values<Slic3r::ConfigOptionFloats, double>(printer_config.option<Slic3r::ConfigOptionFloats>(key), trim_param_indices);
            break;
        }
        case Slic3r::coFloatsOrPercents: {
            trim_option_values<Slic3r::ConfigOptionFloatsOrPercents, Slic3r::FloatOrPercent>(printer_config.option<Slic3r::ConfigOptionFloatsOrPercents>(key), trim_param_indices);
            break;
        }
        case Slic3r::coBools: {
            trim_option_values<Slic3r::ConfigOptionBools, unsigned char>(printer_config.option<Slic3r::ConfigOptionBools>(key), trim_param_indices);
            break;
        }
        case Slic3r::coEnums: {
            trim_option_values<Slic3r::ConfigOptionEnumsGeneric, int>(printer_config.option<Slic3r::ConfigOptionEnumsGeneric>(key), trim_param_indices);
            break;
        }
        default: break;
        }
    }
}

void PrintHooks::update_filament_maps_to_config(
    Slic3r::Print& print,
    const std::vector<int>& f_maps,
    const std::vector<int>& f_volume_maps,
    const std::vector<int>& f_nozzle_maps
)
{
    if ((print.m_config.filament_map.values != f_maps) || (print.m_config.filament_volume_map.values != f_volume_maps) || (print.m_config.filament_nozzle_map.values != f_nozzle_maps))
    {
        VORTEK_LOG(info, "filament maps changed after pre-slicing. Remapping config...");
        print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map", true)->values = f_maps;
        print.m_config.filament_map.values = f_maps;

        if (!f_volume_maps.empty()) {
            print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_volume_map", true)->values = f_volume_maps;
            print.m_config.filament_volume_map.values = f_volume_maps;
        }
        else {
            print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_volume_map", true)->values.resize(f_maps.size(), Slic3r::nvtStandard);
            print.m_config.filament_volume_map.values.resize(f_maps.size(), Slic3r::nvtStandard);
        }

        if (!f_nozzle_maps.empty()) {
            print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)->values = f_nozzle_maps;
            print.m_config.filament_nozzle_map.values = f_nozzle_maps;
        }
    }

    {
        int extruder_count = print.m_config.nozzle_diameter.values.size();
        int extruder_volume_type_count = 1;

        // filament_map_2
        print.m_config.filament_map_2.values = f_maps;
        auto opt_extruder_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(print.m_ori_full_print_config.option("extruder_type"));
        auto opt_nozzle_volume_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(print.m_ori_full_print_config.option("nozzle_volume_type"));
        for (size_t index = 0; index < f_maps.size(); index++)
        {
            Slic3r::ExtruderType extruder_type = Slic3r::etDirectDrive;
            if (opt_extruder_type && index < opt_extruder_type->size())
                extruder_type = (Slic3r::ExtruderType)(opt_extruder_type->get_at(f_maps[index] - 1));
            Slic3r::NozzleVolumeType nozzle_volume_type = Slic3r::nvtStandard;
            if (opt_nozzle_volume_type && index < opt_nozzle_volume_type->size())
                nozzle_volume_type = (Slic3r::NozzleVolumeType)(opt_nozzle_volume_type->get_at(f_maps[index] - 1));
            if (f_volume_maps.empty()) {
                print.m_config.filament_volume_map.values[index] = nozzle_volume_type;
                print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values[index] = nozzle_volume_type;
            }
            else if (print.m_config.filament_volume_map.values.size() > index)
                nozzle_volume_type = (Slic3r::NozzleVolumeType)(print.m_config.filament_volume_map.values[index]);
            print.m_config.filament_map_2.values[index] = print.m_ori_full_print_config.get_index_for_extruder(f_maps[index], "print_extruder_id", extruder_type, nozzle_volume_type, "print_extruder_variant");
        }
        print.m_full_print_config = print.m_ori_full_print_config;

        std::set<std::string> filament_keys = Slic3r::filament_options_with_variant;
        filament_keys.insert("filament_self_index");
        print.m_full_print_config.update_values_to_printer_extruders_for_multiple_filaments(print.m_full_print_config, filament_keys, "filament_self_index", "filament_extruder_variant");

        const std::vector<std::string> &extruder_retract_keys = Slic3r::print_config_def.extruder_retract_keys();
        const std::string               filament_prefix       = "filament_";
        Slic3r::t_config_option_keys            print_diff;
        Slic3r::DynamicPrintConfig              filament_overrides;
        for (auto& opt_key: extruder_retract_keys)
        {
            const Slic3r::ConfigOption *opt_new_filament = print.m_full_print_config.option(filament_prefix + opt_key);
            const Slic3r::ConfigOption *opt_new_machine = print.m_full_print_config.option(opt_key);
            const Slic3r::ConfigOption *opt_old_machine = print.m_config.option(opt_key);

            if (opt_new_filament)
                Slic3r::compute_filament_override_value(opt_key, opt_old_machine, opt_new_machine, opt_new_filament, print.m_full_print_config, print_diff, filament_overrides, print.m_config.filament_map_2.values);
        }

        Slic3r::t_config_option_keys keys(Slic3r::filament_options_with_variant.begin(), Slic3r::filament_options_with_variant.end());
        keys.push_back("filament_self_index");
        print.m_config.apply_only(print.m_full_print_config, keys, true);
        if (!print_diff.empty()) {
            print.m_placeholder_parser.apply_config(filament_overrides);
            print.m_config.apply(filament_overrides);
        }
    }
}

void PrintHooks::update_to_config_by_nozzle_group_result(
    Slic3r::Print& print,
    const Slic3r::MultiNozzleUtils::NozzleGroupResultBase& group_result
)
{
    int extruder_count = print.m_config.nozzle_diameter.values.size();
    VORTEK_LOG(info, "update_to_config_by_nozzle_group_result: carriage count = " << extruder_count);
    int extruder_volume_type_count = 1;

    std::unordered_map<int, std::vector<Slic3r::ExtruderNozleInfo>> filament_extruder_map;

    auto filament_count = print.m_config.option<Slic3r::ConfigOptionStrings>("filament_type")->size();
    auto extruder_type  = print.m_config.option<Slic3r::ConfigOptionEnumsGeneric>("extruder_type")->values;

    for (int fidx = 0; fidx < (int)filament_count; ++fidx) {
        auto used_nozzles = group_result.get_nozzles_for_filament(fidx);
        std::set<Slic3r::ExtruderNozleInfo> extruder_nozzle_set;
        for (auto nozzle : used_nozzles) {
            Slic3r::ExtruderNozleInfo tmp;
            tmp.extruder_type = Slic3r::ExtruderType(extruder_type[nozzle.extruder_id]);
            tmp.nozzle_volume_type = nozzle.volume_type;
            extruder_nozzle_set.insert(tmp);
        }
        filament_extruder_map[fidx] = std::vector<Slic3r::ExtruderNozleInfo>(extruder_nozzle_set.begin(), extruder_nozzle_set.end());
    }

    print.m_full_print_config = print.m_ori_full_print_config;
    std::set<std::string> filament_keys = Slic3r::filament_options_with_variant;
    filament_keys.insert("filament_self_index");
    update_filament_config_values_for_multiple_extruders(print.m_full_print_config, filament_extruder_map, extruder_count, extruder_volume_type_count,
                                                          filament_keys, "filament_self_index", "filament_extruder_variant");

    const std::vector<std::string> &extruder_retract_keys = Slic3r::print_config_def.extruder_retract_keys();
    const std::string               filament_prefix       = "filament_";
    Slic3r::t_config_option_keys            print_diff;
    Slic3r::DynamicPrintConfig              filament_overrides;
    for (auto &opt_key : extruder_retract_keys) {
        const Slic3r::ConfigOption *opt_new_filament = print.m_full_print_config.option(filament_prefix + opt_key);
        const Slic3r::ConfigOption *opt_new_machine  = print.m_full_print_config.option(opt_key);
        const Slic3r::ConfigOption *opt_old_machine  = print.m_config.option(opt_key);

        if (opt_new_filament)
            Slic3r::compute_filament_override_value(opt_key, opt_old_machine, opt_new_machine, opt_new_filament, print.m_full_print_config, print_diff, filament_overrides,
                                            print.m_config.filament_map_2.values);
    }

    Slic3r::t_config_option_keys keys(Slic3r::filament_options_with_variant.begin(), Slic3r::filament_options_with_variant.end());
    keys.push_back("filament_self_index");
    print.m_config.apply_only(print.m_full_print_config, keys, true);
    if (!print_diff.empty()) {
        print.m_placeholder_parser.apply_config(filament_overrides);
        print.m_config.apply(filament_overrides);
    }
}

#ifndef L
#define L(s) (s)
#endif

void PrintHooks::init_vortek_params(Slic3r::PrintConfigDef* def_ptr)
{
    using namespace Slic3r;
    VORTEK_LOG(info, "init_vortek_params: registering 18 Vortek configuration parameters");

    ConfigOptionDef* def = def_ptr->add("extruder_max_nozzle_count", coInts);
    def->mode = comDevelop;
    def->nullable = true;
    def->set_default_value(new ConfigOptionIntsNullable{ 1 });

    def = def_ptr->add("extruder_nozzle_stats", coStrings);
    def->set_default_value(new ConfigOptionStrings { });

    def = def_ptr->add("enable_filament_dynamic_map", coBool);
    def->label = "Enable filament dynamic map";
    def->tooltip = "Support filament map to different nozzle";
    def->set_default_value(new ConfigOptionBool{ false });

    def = def_ptr->add("has_filament_switcher", coBool);
    def->label = "Has filament switcher";
    def->tooltip = "Whether a filament switcher is connected to the printer";
    def->set_default_value(new ConfigOptionBool{ false });

    def = def_ptr->add("prime_volume_mode", coEnum);
    def->enum_values.push_back("Default");
    def->enum_values.push_back("Saving");
    def->enum_values.push_back("Fast");
    def->enum_labels.push_back(L("Default"));
    def->enum_labels.push_back(L("Saving"));
    def->enum_labels.push_back(L("Fast"));
    def->enum_keys_map = &ConfigOptionEnum<PrimeVolumeMode>::get_enum_values();
    def->set_default_value(new ConfigOptionEnum<PrimeVolumeMode>{ PrimeVolumeMode::pvmDefault });

    def = def_ptr->add("machine_hotend_change_time", coFloat);
    def->label = L("Hotend change time");
    def->tooltip = L("Time to change hotend.");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = def_ptr->add("hotend_cooling_rate", coFloats);
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{2});

    def = def_ptr->add("hotend_heating_rate", coFloats);
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{2});

    def = def_ptr->add("enable_pre_heating", coBool);
    def->set_default_value(new ConfigOptionBool(false));

    def = def_ptr->add("filament_nozzle_map", coInts);
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{1});

    def = def_ptr->add("filament_volume_map", coInts);
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{(int)(NozzleVolumeType::nvtStandard)});

    def = def_ptr->add("filament_map_2", coInts);
    def->label = "Filament map plus for multi nozzle";
    def->tooltip = "Filament map to the index identified by extruder and nozzle_volume_type";
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{1});

    def = def_ptr->add("filament_pre_cooling_temperature_nc", coInts);
    def->mode = comAdvanced;
    def->sidetext = "°C";
    def->min = 0;
    def->nullable = true;
    def->set_default_value(new ConfigOptionIntsNullable{0});

    def = def_ptr->add("filament_ramming_volumetric_speed_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("The maximum volumetric speed for ramming before a hotend change, where -1 means using the maximum volumetric speed.");
    def->sidetext = L("mm³/s");
    def->min = -1;
    def->max = 200;
    def->mode = comAdvanced;
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{-1});

    def = def_ptr->add("filament_ramming_travel_time_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("To prevent oozing, the nozzle will perform a reverse travel movement for a certain period after the ramming is complete. The setting define the travel time.");
    def->sidetext = "s";
    def->min = 0;
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{0});

    def = def_ptr->add("filament_change_length_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("When changing the hotend, it is recommended to extrude a certain length of filament from the original nozzle. This helps minimize nozzle oozing.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{10});

    def = def_ptr->add("filament_prime_volume", coFloats);
    def->label = L("Filament change");
    def->tooltip = L("The volume of material required to prime the extruder on the tower, excluding a hotend change.");
    def->sidetext = L("mm³");
    def->min = 1.0;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloats{45.});

    def = def_ptr->add("filament_prime_volume_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("The volume of material required to prime the extruder for a hotend change on the tower.");
    def->sidetext = L("mm³");
    def->min = 1.0;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloats{60.});

    def = def_ptr->add("filament_retract_length_nc", coFloats);
    def->label = L("Nozzle Changer retraction length");
    def->tooltip = L("The length of retraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{0.0});

    def = def_ptr->add("filament_retract_lift_nc", coFloats);
    def->label = L("Nozzle Changer retract lift");
    def->tooltip = L("The Z-hop distance when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{0.0});

    def = def_ptr->add("filament_retract_speed_nc", coInts);
    def->label = L("Nozzle Changer retract speed");
    def->tooltip = L("The speed of retraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInts{0});

    def = def_ptr->add("filament_deretract_speed_nc", coInts);
    def->label = L("Nozzle Changer deretract speed");
    def->tooltip = L("The speed of deretraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInts{0});

    // Register custom filament keys in the global filament options set
    Slic3r::filament_options_with_variant.insert("filament_pre_cooling_temperature_nc");
    Slic3r::filament_options_with_variant.insert("filament_ramming_volumetric_speed_nc");
    Slic3r::filament_options_with_variant.insert("filament_ramming_travel_time_nc");
    Slic3r::filament_options_with_variant.insert("filament_change_length_nc");
    Slic3r::filament_options_with_variant.insert("filament_prime_volume");
    Slic3r::filament_options_with_variant.insert("filament_prime_volume_nc");
    Slic3r::filament_options_with_variant.insert("filament_retract_length_nc");
    Slic3r::filament_options_with_variant.insert("filament_retract_lift_nc");
    Slic3r::filament_options_with_variant.insert("filament_retract_speed_nc");
    Slic3r::filament_options_with_variant.insert("filament_deretract_speed_nc");
}

std::vector<int> PrintHooks::get_filament_nozzle_maps(const Slic3r::Print& print)
{
    if (print.m_config.has("filament_nozzle_map")) {
        return print.m_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    }
    return {};
}

std::vector<int> PrintHooks::get_filament_volume_maps(const Slic3r::Print& print)
{
    if (print.m_config.has("filament_volume_map")) {
        return print.m_config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    }
    return {};
}

#undef L

} // namespace Vortek

