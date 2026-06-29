#include "VortekUtilBackend.h"
#include "VortekNozzleRack.h"
#include "DevNozzleSystem.h"

#include "DevUtil.h"
#include "VortekDeviceHooks.hpp"

#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/Print.hpp"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

#include "slic3r/GUI/GUI_App.hpp"

#include <boost/lexical_cast.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

MultiNozzleUtils::NozzleInfo VortekUtilBackend::GetNozzleInfo(const DevNozzle& dev_nozzle)
{
    MultiNozzleUtils::NozzleInfo info;
    info.diameter = Vortek::DeviceHooks::get_nozzle_diameter_str(dev_nozzle);
    info.volume_type = (Vortek::DeviceHooks::get_nozzle_flow_type(dev_nozzle) == NozzleFlowType::H_FLOW ? NozzleVolumeType::nvtHighFlow : NozzleVolumeType::nvtStandard);
    info.extruder_id = Vortek::DeviceHooks::get_logic_extruder_id(dev_nozzle);

    return info;
}

std::shared_ptr<Slic3r::MultiNozzleUtils::NozzleGroupResultBase> VortekUtilBackend::GetNozzleGroupResult(Slic3r::GUI::Plater *plater)
{
    if (plater) {
        if (plater->get_partplate_list().get_current_slice_result()) {
            if (auto res = plater->fff_print().get_nozzle_group_result())
                return res;
        }
    }

    return nullptr;
}

std::unordered_map<NozzleDef, int> VortekUtilBackend::CollectNozzleInfo(MultiNozzleUtils::NozzleGroupResultBase *nozzle_group_res, int logic_ext_id)
{
    std::unordered_map<NozzleDef, int> need_nozzle_map;
    if (!nozzle_group_res) {
        return need_nozzle_map;
    }

    const std::vector<Slic3r::MultiNozzleUtils::NozzleInfo>& nozzle_vec = nozzle_group_res->get_used_nozzles_in_extruder(logic_ext_id);
    for (auto slicing_nozzle : nozzle_vec) {
        try {
            NozzleDef data;
            data.nozzle_diameter = boost::lexical_cast<float>(slicing_nozzle.diameter);
            data.nozzle_flow_type = (slicing_nozzle.volume_type == NozzleVolumeType::nvtHighFlow ? NozzleFlowType::H_FLOW : NozzleFlowType::S_FLOW);
            need_nozzle_map[data]++;
        } catch (const std::exception& e) {
            assert(0);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "exception: " << e.what();
        }
    }

    return need_nozzle_map;
}

static std::unordered_map<std::string, DevAmsType> s_ams_type_map = {
    {"0", DevAmsType::N3F},
    {"1", DevAmsType::N3S},
    {"2", DevAmsType::N3S},
    {"3", DevAmsType::AMS_LITE_MIXED}
};

std::optional<Slic3r::DevFilamentDryingPreset> VortekUtilBackend::GetFilamentDryingPreset(const std::string& fila_id)
{
    if (fila_id.empty() || !GUI::wxGetApp().preset_bundle) {
        return std::nullopt;
    }

    for (auto iter = GUI::wxGetApp().preset_bundle->filaments.begin(); iter != GUI::wxGetApp().preset_bundle->filaments.end(); ++iter) {
        const Preset& filament_preset = *iter;
        const auto& config = filament_preset.config;
        if (filament_preset.filament_id == fila_id) {
            DevFilamentDryingPreset info;
            info.filament_id = fila_id;
            try {
                if (config.has("filament_dev_ams_drying_ams_limitations")) {
                    std::vector<std::string> types = config.option<ConfigOptionStrings>("filament_dev_ams_drying_ams_limitations")->values;
                    for (auto type : types) {
                        if (s_ams_type_map.count(type) == 0) {
                            continue;
                        }
                        info.ams_limitations.insert(s_ams_type_map[type]);
                    }
                }

                if (config.has("filament_dev_ams_drying_temperature")) {
                    info.filament_dev_ams_drying_temperature_on_idle[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(0);
                    info.filament_dev_ams_drying_temperature_on_idle[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(1);
                    info.filament_dev_ams_drying_temperature_on_print[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(2);
                    info.filament_dev_ams_drying_temperature_on_print[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_temperature")->get_at(3);
                }

                if (config.has("filament_dev_ams_drying_time")) {
                    info.filament_dev_ams_drying_time_on_idle[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(0);
                    info.filament_dev_ams_drying_time_on_idle[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(1);
                    info.filament_dev_ams_drying_time_on_print[DevAmsType::N3F] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(2);
                    info.filament_dev_ams_drying_time_on_print[DevAmsType::N3S] = config.option<ConfigOptionFloats>("filament_dev_ams_drying_time")->get_at(3);
                }

                if (config.has("filament_dev_drying_softening_temperature")) {
                    info.filament_dev_drying_softening_temperature =  config.option<ConfigOptionFloats>("filament_dev_drying_softening_temperature")->get_at(0);
                }

                if (config.has("filament_dev_ams_drying_heat_distortion_temperature")){
                    info.filament_dev_ams_drying_heat_distortion_temperature = config.option<ConfigOptionFloats>("filament_dev_ams_drying_heat_distortion_temperature")->get_at(0);
                }

                if (config.has("filament_dev_drying_cooling_temperature")) {
                    info.filament_dev_drying_cooling_temperature = config.option<ConfigOptionFloats>("filament_dev_drying_cooling_temperature")->get_at(0);
                }

                return info;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception: " << e.what();
            }
        }
    }

    return std::nullopt;
}

} // namespace Slic3r
