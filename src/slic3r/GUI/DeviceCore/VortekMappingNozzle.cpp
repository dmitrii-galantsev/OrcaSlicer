#include "DevMapping.h"
#include "VortekMappingNozzle.h"

#include "VortekNozzleRack.h"
#include "DevNozzleSystem.h"

#include "DevUtil.h"
#include "VortekUtilBackend.h"

#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/Print.hpp"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "DevDefs.h"
#include "VortekDeviceHooks.hpp"

#include "slic3r/GUI/GUI_App.hpp"

#include <boost/lexical_cast.hpp>
#include <nlohmann/json.hpp>
using namespace nlohmann;

namespace Slic3r {

static std::string  s_get_diameter_str(float diameter)
{
    return (boost::format("%.2f") % diameter).str();
}

static std::string s_get_diameter_str(const std::string& diameter)
{
    try {
        float dia = boost::lexical_cast<float>(diameter);
        return s_get_diameter_str(dia);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to boost::lexical_cast: " << diameter;
        return diameter;
    }

    try {
        float dia = std::stof(diameter);
        return s_get_diameter_str(dia);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " std::stof: " << diameter;
        return diameter;
    }
}


// get auto nozzle mapping through AP
// warnings:
// (1) the fila_id here is 1 based index
// (2) the AP wants the diameter string with 2 decimal places
int VortekNozzleMappingCtrl::CtrlGetAutoNozzleMappingV0(Slic3r::GUI::Plater* plater,
                                                     const std::vector<FilamentInfo>& ams_mapping,
                                                     int flow_cali_opt, int pa_value)
{
    Clear();

    m_plater = plater;
    if (!plater) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": plater is nullptr";
        return -1;
    }

    GCodeProcessorResult* gcode_result = plater->get_partplate_list().get_current_slice_result();
    if (!gcode_result) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": gcode_result is nullptr";
        return -1;
    }

    const auto& result = plater->fff_print().get_nozzle_group_result();
    if (!result) {
        assert(false && "plater->fff_print().get_nozzle_group_result()");
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get_nozzle_group_result is NULL";
        return -1;
    }

    if (ams_mapping.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": the ams mapping is empty";
        return -1;// the ams mapping is empty
    }

    json command_jj;
    command_jj["print"]["command"] = "get_auto_nozzle_mapping";
    m_sequence_id = std::to_string(MachineObject::m_sequence_id++);
    command_jj["print"]["sequence_id"] = m_sequence_id;
    command_jj["print"]["calibration"] = flow_cali_opt;
    command_jj["print"]["extrude_cali_manual_mode"] = pa_value;

    // filament seq
    json filament_seq_jj;
    int max_fila_id = 0;
    std::unordered_set<int> filaid_set;
    for (int idx = 0; idx < gcode_result->filament_change_sequence.size(); idx++) {
        int fila_id = gcode_result->filament_change_sequence[idx];
        if (filaid_set.count(fila_id) == 0) {
            filaid_set.insert(fila_id);
            filament_seq_jj[fila_id + 1] = idx;
            max_fila_id = std::max(max_fila_id, fila_id + 1);
        }
    }

    for (int fila_id = 0; fila_id <= max_fila_id; fila_id++) {
        if (filament_seq_jj[fila_id].is_null()) {
            filament_seq_jj[fila_id] = -1;// fill the used fila_id with -1
        }
    }

    command_jj["print"]["filament_seq"] = filament_seq_jj;

    // ams mapping
    std::vector<int> ams_mapping_vec(33, 0xFFFF);/* AP ask to fill them*/
    for (auto item :  ams_mapping) {
        try {
            int ams_id = stoi(item.ams_id);
            int slot_id = !item.slot_id.empty() ? stoi(item.slot_id) : 0;
            ams_mapping_vec[item.id + 1] = (ams_id << 8) | slot_id;/*using ams_id << 8 | slot_id*/
        } catch (std::exception& e) {
            assert(false && "invalid ams_id or slot_id");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception: " << e.what();
            ams_mapping_vec[item.id + 1] = item.tray_id;
        }
    }
    command_jj["print"]["ams_mapping"] = ams_mapping_vec;

    // filament info
    json filament_info_jj;
    for (int fila_id = 0; fila_id < ams_mapping.size(); fila_id++) {
        const auto& fila = ams_mapping[fila_id];
        const auto& nozzle_list = result->get_nozzles_for_filament(fila.id);
        if (nozzle_list.empty()) {
            assert(false && "nozzle_list should not be empty");
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "nozzle_list should not be nullptr";
            continue;
        }

        for (const auto& nozzle_info : nozzle_list) {
            json fila_item_jj;
            fila_item_jj["id"] = (fila.id + 1);
            fila_item_jj["direction"] = (nozzle_info.extruder_id == LOGIC_L_EXTRUDER_ID ? 1 : 2);
            fila_item_jj["group"] = nozzle_info.group_id;
            fila_item_jj["nozzle_d"] = s_get_diameter_str(nozzle_info.diameter);
            fila_item_jj["nozzle_v"] = (nozzle_info.volume_type == NozzleVolumeType::nvtHighFlow) ? "High Flow" : "Standard";
            fila_item_jj["cate"] = fila.filament_id;
            fila_item_jj["color"] = fila.color;
            filament_info_jj.push_back(fila_item_jj);
        }
    }

    command_jj["print"]["fila_info"] = filament_info_jj;

    // nozzle info
    json nozzle_info_jj;
    const auto& nozzle_system = m_obj->GetNozzleSystem();
    const auto& extruder_nozzles = nozzle_system->GetNozzles();
    for (const auto& nozzle : extruder_nozzles) {
        if (Vortek::DeviceHooks::is_nozzle_normal(nozzle.second)) {
            json nozzle_item_jj;
            nozzle_item_jj["pos"] = Vortek::DeviceHooks::get_nozzle_id(nozzle.second);
            auto tar = Vortek::DeviceHooks::get_replace_nozzle_tar(nozzle_system);
            if (tar.has_value() && nozzle_item_jj["pos"] == MAIN_EXTRUDER_ID) {
                nozzle_item_jj["pos"] = tar.value();// special case of tar_id. see protocol definition
            }

            nozzle_item_jj["nozzle_d"] = s_get_diameter_str(nozzle.second.m_diameter);
            nozzle_item_jj["nozzle_v"] = Vortek::DeviceHooks::to_nozzle_flow_string(Vortek::DeviceHooks::get_nozzle_flow_type(nozzle.second));
            nozzle_item_jj["wear"] = Vortek::DeviceHooks::get_nozzle_wear(nozzle.second);
            nozzle_item_jj["cate"] = Vortek::DeviceHooks::get_nozzle_filament_id(nozzle.second, nozzle_system);
            nozzle_item_jj["color"] = Vortek::DeviceHooks::get_nozzle_filament_color(nozzle.second, nozzle_system);
            nozzle_info_jj.push_back(nozzle_item_jj);
        }
    }

    auto rack = Vortek::DeviceHooks::get_nozzle_rack(nozzle_system);
    const auto& rack_nozzles = rack ? rack->GetRackNozzles() : std::map<int, DevNozzle>();
    for (const auto& nozzle : rack_nozzles) {
        if (Vortek::DeviceHooks::is_nozzle_normal(nozzle.second)) {
            json nozzle_item_jj;
            nozzle_item_jj["pos"] = (Vortek::DeviceHooks::get_nozzle_id(nozzle.second) + 0x10);
            auto tar = Vortek::DeviceHooks::get_replace_nozzle_tar(nozzle_system);
            if (tar.has_value() && nozzle_item_jj["pos"] == MAIN_EXTRUDER_ID) {
                nozzle_item_jj["pos"] = tar.value();// special case of tar_id. see protocol definition
            }

            nozzle_item_jj["nozzle_d"] = s_get_diameter_str(nozzle.second.m_diameter);
            nozzle_item_jj["nozzle_v"] = Vortek::DeviceHooks::to_nozzle_flow_string(Vortek::DeviceHooks::get_nozzle_flow_type(nozzle.second));
            nozzle_item_jj["wear"] = Vortek::DeviceHooks::get_nozzle_wear(nozzle.second);
            nozzle_item_jj["cate"] = Vortek::DeviceHooks::get_nozzle_filament_id(nozzle.second, nozzle_system);
            nozzle_item_jj["color"] = Vortek::DeviceHooks::get_nozzle_filament_color(nozzle.second, nozzle_system);
            nozzle_info_jj.push_back(nozzle_item_jj);
        }
    }

    command_jj["print"]["nozzle_info"] = nozzle_info_jj;
    m_req_version = NozzleMappingVersion::Version_V0;
    return m_obj->publish_json(command_jj);
}

int VortekNozzleMappingCtrl::CtrlGetAutoNozzleMappingV1(Slic3r::GUI::Plater* plater)
{
    Clear();
    m_plater = plater;
    if (!m_plater) {
        return -1;
    }

    auto nozzle_group_res = VortekUtilBackend::GetNozzleGroupResult(m_plater);
    if (!nozzle_group_res) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get_nozzle_group_result is NULL";
        return -1;
    }

    json command_jj;
    command_jj["print"]["command"] = "get_auto_nozzle_mapping";
    m_sequence_id = std::to_string(MachineObject::m_sequence_id++);
    command_jj["print"]["sequence_id"] = m_sequence_id;

    json nozzle_group_info_jj;
    std::unordered_set<int> used_logic_groups;
    const auto& used_logic_nozzles = nozzle_group_res->get_used_nozzles_in_extruder();
    for (const auto& used_logic_nozzle : used_logic_nozzles) {
        if (used_logic_groups.count(used_logic_nozzle.group_id) != 0) {
            continue;
        }
        used_logic_groups.insert(used_logic_nozzle.group_id);

        json nozzle_info;
        nozzle_info["id"] = used_logic_nozzle.group_id;
        nozzle_info["ext"] = (used_logic_nozzle.extruder_id + 1);

        try {
            nozzle_info["dia"] = std::stof(used_logic_nozzle.diameter);
        } catch(const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": e=" << e.what();
            continue;
        }

        if (used_logic_nozzle.volume_type == NozzleVolumeType::nvtHighFlow) {
            nozzle_info["vol"] = "High Flow";
        } else if(used_logic_nozzle.volume_type == NozzleVolumeType::nvtStandard){
            nozzle_info["vol"] = "Standard";
        } else {
            assert(0);
            continue;
        }

        nozzle_group_info_jj.push_back(nozzle_info);
    }
    command_jj["print"]["group_info"] = nozzle_group_info_jj;


    m_req_version = NozzleMappingVersion::Version_V1;
    command_jj["print"]["version"] = (int)m_req_version;
    return m_obj->publish_json(command_jj);
}


void VortekNozzleMappingCtrl::ParseAutoNozzleMapping(const json& print_jj)
{
    if (print_jj.contains("command") && print_jj["command"].get<string>() == "get_auto_nozzle_mapping") {
        if (print_jj.contains("sequence_id") && print_jj["sequence_id"] == m_sequence_id) {
            Clear();
            DevJsonValParser::ParseVal(print_jj, "result", m_result);
            DevJsonValParser::ParseVal(print_jj, "reason", m_mqtt_reason);
            DevJsonValParser::ParseVal(print_jj, "errno", m_errno);
            DevJsonValParser::ParseVal(print_jj, "detail", m_detail_json);
            DevJsonValParser::ParseVal(print_jj, "type", m_type);
            DevJsonValParser::ParseVal(print_jj, "version", m_ack_version);

            if (print_jj.contains("mapping")) {
                m_nozzle_mapping_json = print_jj["mapping"];
                const auto& mapping = print_jj["mapping"].get<std::vector<int>>();
                for (int fila_id = 0; fila_id < mapping.size(); ++fila_id) {
                    m_nozzle_mapping[fila_id] = mapping[fila_id];
                }
            }

            m_flush_weight_base = GetFlushWeight(m_obj);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get_auto_nozzle_mapping: " << m_result;
        }
    }
}

void VortekNozzleMappingCtrl::Clear()
{
    m_sequence_id.clear();
    m_result.clear();
    m_mqtt_reason.clear();
    m_type.clear();
    m_errno = 0;
    m_detail_msg.clear();
    m_detail_json.clear();
    m_nozzle_mapping.clear();
    m_nozzle_mapping_json.clear();

    m_flush_weight_base = -1;
    m_flush_weight_current = -1;
    m_ack_version = NozzleMappingVersion::Version_V0;
}


void VortekNozzleMappingCtrl::SetManualNozzleMappingByFila(int fila_id, int nozzle_pos_id)
{
    auto tar = Vortek::DeviceHooks::get_replace_nozzle_tar(m_obj->GetNozzleSystem());
    if (nozzle_pos_id == MAIN_EXTRUDER_ID && tar.has_value()){
        nozzle_pos_id = tar.value();// special case of tar_id. see protocol definition
    }

    if (m_nozzle_mapping[fila_id] != nozzle_pos_id) {
        m_nozzle_mapping[fila_id] = nozzle_pos_id;
        m_flush_weight_current = GetFlushWeight(m_obj);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": fila_id=" << fila_id << ", nozzle_pos_id=" << nozzle_pos_id;
    }

    try {
        auto mapping = m_nozzle_mapping_json.get<std::vector<int>>();
        if (mapping.at(fila_id) != nozzle_pos_id) {
            mapping[fila_id] = nozzle_pos_id;
            m_nozzle_mapping_json = mapping;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": updated to " << m_nozzle_mapping_json.dump();
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": exception: " << e.what();
    };
}

static std::unordered_set<int> sGetLogicExtruders(const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_infos)
{
    std::unordered_set<int> extruder_ids;
    for (const auto& nozzle : nozzle_infos) {
        if (nozzle.extruder_id >= 0) {
            extruder_ids.insert(nozzle.extruder_id);
        }
    }

    return extruder_ids;
};


std::vector<int> VortekNozzleMappingCtrl::GetMappedNozzlePosVecByFilaId(int fila_id) const
{
    std::vector<int> physic_nozzle_pos_vec;
    auto nozzle_group_res = VortekUtilBackend::GetNozzleGroupResult(m_plater);
    if (!nozzle_group_res) {
        return physic_nozzle_pos_vec;
    }

    const auto& used_logic_nozzles = nozzle_group_res->get_nozzles_for_filament(fila_id);
    if (m_ack_version == NozzleMappingVersion::Version_V0) {
        if (auto iter = m_nozzle_mapping.find(fila_id); iter != m_nozzle_mapping.end()) {
            auto tar = Vortek::DeviceHooks::get_replace_nozzle_tar(m_obj->GetNozzleSystem());
            if (tar.has_value() && iter->second == tar.value()) {
                physic_nozzle_pos_vec.push_back(MAIN_EXTRUDER_ID);
            } else {
                physic_nozzle_pos_vec.push_back(iter->second);
            }
        } else {
            const auto& used_logic_extruders = sGetLogicExtruders(used_logic_nozzles);
            if (used_logic_extruders.size() == 1 && *used_logic_extruders.begin() == LOGIC_L_EXTRUDER_ID) {
                physic_nozzle_pos_vec.push_back(1);
                return physic_nozzle_pos_vec; // only left extruder is used
            }
        }
    } else if (m_ack_version == NozzleMappingVersion::Version_V1) {
        for (const auto& used_logic_nozzle : used_logic_nozzles) {
            if (auto iter = m_nozzle_mapping.find(used_logic_nozzle.group_id); iter != m_nozzle_mapping.end()) {
                auto tar = Vortek::DeviceHooks::get_replace_nozzle_tar(m_obj->GetNozzleSystem());
                if (tar.has_value() && iter->second == tar.value()) {
                    physic_nozzle_pos_vec.push_back(MAIN_EXTRUDER_ID);
                } else {
                    physic_nozzle_pos_vec.push_back(iter->second);
                }
            }
        }
    }

    return physic_nozzle_pos_vec;
}

wxString VortekNozzleMappingCtrl::GetMappedNozzlePosStrByFilaId(int fila_id, const wxString& default_str) const
{
    wxString display_str;
    const auto& physic_nozzle_pos_vec = GetMappedNozzlePosVecByFilaId(fila_id);
    for (int idx = 0; idx < physic_nozzle_pos_vec.size(); idx++) {
        int pos_id = physic_nozzle_pos_vec[idx];
        if (pos_id == MAIN_EXTRUDER_ID) {
            display_str += "R";
        } else if (pos_id == DEPUTY_EXTRUDER_ID) {
            display_str += "L";
        } else if (pos_id >= 0x10) {
            display_str += wxString::Format("R%d", pos_id - 0x10 + 1);// display 1~n for rack hotends
        } else {
            continue;
        }

        if (idx < physic_nozzle_pos_vec.size() - 1) {
            display_str += " ";
        }
    }

    return !display_str.empty() ? display_str : default_str;
}

float VortekNozzleMappingCtrl::GetFlushWeight(Slic3r::MachineObject* obj) const
{
    auto plater = Slic3r::GUI::wxGetApp().plater();
    if (!plater) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": plater is nullptr";
        return -1;
    }

    GCodeProcessorResult* gcode_result = plater->get_partplate_list().get_current_slice_result();
    if (!gcode_result) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": gcode_result is nullptr";
        return -1;
    }

    if (gcode_result->filament_change_sequence.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": filament_change_sequence is empty";
        return -1;
    }

    if (!Slic3r::GUI::wxGetApp().preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": preset_bundle is nullptr";
        return -1;
    };

    const std::vector<std::vector<std::vector<float>>>& flush_matrix = Vortek::DeviceHooks::get_full_flush_matrix_helper(Slic3r::GUI::wxGetApp().preset_bundle);

    float total_flush_volume = 0;
    MultiNozzleUtils::NozzleStatusRecorder recorder;
    for (auto filament : gcode_result->filament_change_sequence) {
        const auto& nozzle_pos_vec = GetMappedNozzlePosVecByFilaId(filament);
        if (nozzle_pos_vec.empty()) {
            continue;
        }

        auto nozzle_pos = nozzle_pos_vec.at(0);
        if (nozzle_pos == -1){
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": nozzle_pos is -1 for fila_id=" << filament;
            continue;
        }

        auto nozzle_info = Vortek::DeviceHooks::get_nozzle_by_pos_id(obj->GetNozzleSystem(), nozzle_pos);
        if (Vortek::DeviceHooks::is_nozzle_empty(nozzle_info)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": nozzle_info IsEmpty for fila_id=" << filament << ", nozzle_pos=" << nozzle_pos;
            continue;
        }

        int extruder_id = Vortek::DeviceHooks::get_logic_extruder_id(nozzle_info);
        int nozzle_id = Vortek::DeviceHooks::get_nozzle_pos_id(nozzle_info, obj->GetNozzleSystem());
        int last_filament = recorder.get_filament_in_nozzle(nozzle_id);

        if (last_filament != -1 && last_filament != filament) {
            if (flush_matrix.size() > extruder_id &&
                flush_matrix[extruder_id].size() > last_filament &&
                flush_matrix[extruder_id][last_filament].size() > filament){
                total_flush_volume += flush_matrix[extruder_id][last_filament][filament];
            }
            else{
                assert(false && "missing flush volume");
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": missing flush volume for extruder_id=" << extruder_id
                                           << ", last_filament=" << last_filament << ", filament=" << filament;
            }
        }

        recorder.set_nozzle_status(nozzle_id, filament);
    }

    return total_flush_volume * 1.26 * 0.001;
}

} // namespace Slic3r
