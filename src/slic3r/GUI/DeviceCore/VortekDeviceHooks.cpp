#include "VortekDeviceHooks.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/VortekNozzleRack.h"
#include "slic3r/GUI/DeviceCore/VortekFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/VortekMappingNozzle.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevUtil.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/VortekLog.hpp"

#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <limits>

namespace Vortek {
namespace DeviceHooks {

static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekNozzleRack>> s_nozzle_racks;
static std::map<const Slic3r::DevAms*, std::set<int>> s_ams_binded_extruders;
static std::map<const Slic3r::DevAms*, std::optional<int>> s_ams_binded_switcher_pos;
static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekNozzleMappingCtrl>> s_nozzle_mappings;
static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekFilaSwitch>> s_fila_switches;

bool is_nozzle_empty(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id == -1 || nozzle.m_nozzle_type == Slic3r::ntUndefine || nozzle.m_diameter < 0.01f;
}

bool is_nozzle_unknown(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id == -1 || nozzle.m_nozzle_type == Slic3r::ntUndefine;
}

bool is_nozzle_info_reliable(const Slic3r::DevNozzle& nozzle) {
    return !is_nozzle_unknown(nozzle) && nozzle.m_diameter > 0.01f;
}

bool is_nozzle_abnormal(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_diameter < 0.01f;
}

std::string get_nozzle_diameter_str(const Slic3r::DevNozzle& nozzle) {
    return std::to_string(nozzle.m_diameter);
}

Slic3r::NozzleFlowType get_nozzle_flow_type(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_flow;
}

int get_logic_extruder_id(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id;
}

uint32_t get_flag_bits_no_border(const std::string& str, int start_idx, int count)
{
    if (start_idx < 0 || count <= 0) return 0;

    try {
        std::string hex = str;
        // --- 1) trim ---
        auto ltrim = [](std::string &s) { s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); })); };
        auto rtrim = [](std::string &s) { s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end()); };
        ltrim(hex);
        rtrim(hex);

        // --- 2) remove 0x/0X prefix ---
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) { hex.erase(0, 2); }

        // --- 3) keep only hex digits ---
        std::string hex_digits;
        hex_digits.reserve(hex.size());
        for (char c : hex) {
            if (std::isxdigit(static_cast<unsigned char>(c))) hex_digits.push_back(c);
        }
        if (hex_digits.empty()) return 0;

        // --- 4) use size_t for all index/bit math ---
        const size_t total_bits = hex_digits.size() * 4ULL;

        const size_t ustart = static_cast<size_t>(start_idx);
        if (ustart >= total_bits) return 0;

        const int    int_bits  = std::numeric_limits<uint32_t>::digits; // typically 32
        const size_t need_bits = static_cast<size_t>(std::min(count, int_bits));

        // [first_bit, last_bit]
        const size_t first_bit = ustart;
        const size_t last_bit  = std::min(ustart + need_bits, total_bits) - 1ULL;
        if (last_bit < first_bit) return 0;

        const size_t right_index = hex_digits.size() - 1ULL;

        const size_t first_nibble = first_bit / 4ULL;
        const size_t last_nibble  = last_bit / 4ULL;

        const size_t start_nibble_idx = right_index - last_nibble;
        const size_t end_nibble_idx   = right_index - first_nibble;
        if (end_nibble_idx < start_nibble_idx) return 0;

        const size_t sub_len = end_nibble_idx - start_nibble_idx + 1ULL;
        if (end_nibble_idx >= hex_digits.size()) return 0;

        const std::string sub_hex = hex_digits.substr(start_nibble_idx, sub_len);

        unsigned long long chunk = std::stoull(sub_hex, nullptr, 16);

        const unsigned           nibble_offset = static_cast<unsigned>(first_bit % 4ULL);
        const unsigned long long shifted       = (nibble_offset == 0U) ? chunk : (chunk >> nibble_offset);

        uint32_t mask;
        if (need_bits >= static_cast<size_t>(std::numeric_limits<uint32_t>::digits)) {
            mask = std::numeric_limits<uint32_t>::max();
        } else {
            mask = static_cast<uint32_t>((1ULL << need_bits) - 1ULL);
        }

        return static_cast<uint32_t>(shifted & mask);
    } catch (...) {
        return 0;
    }
}

bool handle_ams_extruder_binding(
    const Slic3r::MachineObject* obj,
    const json& ams_item,
    int& extruder_id,
    std::set<int>& binded_extruder_set,
    std::optional<int>& binded_switcher_pos)
{
    if (extruder_id == 0xE) {
        auto fs = get_fila_switch(obj);
        if (obj && fs && fs->IsInstalled()) {
            extruder_id = MAIN_EXTRUDER_ID;
            if (ams_item.contains("info")) {
                const std::string& info = ams_item["info"].get<std::string>();
                int bind_switch_in = Slic3r::DevUtil::get_flag_bits(info, 24, 4);
                if (bind_switch_in == 0 || bind_switch_in == 1) {
                    binded_extruder_set = { MAIN_EXTRUDER_ID, DEPUTY_EXTRUDER_ID };
                }
                if (bind_switch_in == 0) {
                    binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_B;
                } else if (bind_switch_in == 1) {
                    binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_A;
                }
                VORTEK_LOG(info, "handle_ams_extruder_binding: mapped 0xE to MAIN/DEPUTY, SwitchPos=" 
                           << (binded_switcher_pos.has_value() ? std::to_string(binded_switcher_pos.value()) : "nullopt"));
            }
            return true;
        } else {
            VORTEK_LOG(warning, "handle_ams_extruder_binding: 0xE detected but FTS is not installed, ignoring AMS");
            return false;
        }
    } else {
        binded_extruder_set = { extruder_id };
        return true;
    }
}

void assign_ams_bindings(
    Slic3r::DevAms* curr_ams,
    const std::set<int>& binded_extruder_set,
    const std::optional<int>& binded_switcher_pos)
{
    if (!curr_ams) return;
    s_ams_binded_extruders[curr_ams] = binded_extruder_set;
    s_ams_binded_switcher_pos[curr_ams] = binded_switcher_pos;
}

void process_nozzle_placement(
    Slic3r::DevNozzleSystem* system,
    Slic3r::DevNozzle& nozzle_obj,
    int raw_id)
{
    if (!system) return;

    int physical_id = Slic3r::DevUtil::get_hex_bits(raw_id, 0);
    int is_on_rack = Slic3r::DevUtil::get_hex_bits(raw_id, 1);

    nozzle_obj.m_nozzle_id = physical_id;
    auto rack = get_nozzle_rack(system);
    if (rack) {
        rack->SetNozzleOnRack(physical_id, is_on_rack == 1);
    }

    if (is_on_rack == 1) {
        if (rack) {
            rack->AddRackNozzle(nozzle_obj);
            VORTEK_LOG(info, "process_nozzle_placement: added nozzle id=" << physical_id << " to rack");
        }
    } else {
        VORTEK_LOG(info, "process_nozzle_placement: added active head nozzle id=" << physical_id);
    }
}

void sync_machine_nozzle_inventory_to_preset(const Slic3r::MachineObject* obj, Slic3r::PresetBundle& preset_bundle)
{
    if (!obj || !obj->GetNozzleSystem()) {
        return;
    }

    auto nozzle_system = obj->GetNozzleSystem();
    auto nozzle_rack = get_nozzle_rack(nozzle_system);
    auto& stat = preset_bundle.extruder_nozzle_stat;

    // Only update if not overridden by user
    stat.set_nozzle_data_flag(Slic3r::ExtruderNozzleStat::ndfMachine);

    // Get number of extruders of active printer preset
    const Slic3r::Preset& current_printer = preset_bundle.printers.get_selected_preset();
    auto* nozzle_diameter_opt = static_cast<const Slic3r::ConfigOptionFloats*>(current_printer.config.option("nozzle_diameter"));
    if (!nozzle_diameter_opt) {
        return;
    }
    int num_extruders = nozzle_diameter_opt->values.size();

    VORTEK_LOG(info, "sync_machine_nozzle_inventory_to_preset: starting sync for " << num_extruders << " extruders");

    if (nozzle_rack && nozzle_rack->IsSupported()) {
        // Nozzle rack is supported (Vortek tool-changer)
        // Count nozzles of each volume type on the rack + the active nozzles in toolhead(s)
        std::map<Slic3r::NozzleVolumeType, int> counts;

        // Iterate through rack nozzles
        for (const auto& pair : nozzle_rack->GetRackNozzles()) {
            const auto& dev_nozzle = pair.second;
            Slic3r::NozzleVolumeType vol_type = Slic3r::nvtStandard;
            if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                vol_type = Slic3r::nvtHighFlow;
            }
            counts[vol_type]++;
            VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: found rack nozzle id=" 
                       << dev_nozzle.m_nozzle_id << ", diameter=" << dev_nozzle.m_diameter 
                       << ", flow=" << (int)dev_nozzle.m_nozzle_flow);
        }

        // Iterate through active toolhead nozzles
        for (const auto& pair : nozzle_system->GetNozzles()) {
            const auto& dev_nozzle = pair.second;
            Slic3r::NozzleVolumeType vol_type = Slic3r::nvtStandard;
            if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                vol_type = Slic3r::nvtHighFlow;
            }
            counts[vol_type]++;
            VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: found active head nozzle id=" 
                       << dev_nozzle.m_nozzle_id << ", diameter=" << dev_nozzle.m_diameter 
                       << ", flow=" << (int)dev_nozzle.m_nozzle_flow);
        }

        // Update extruder nozzle stats for each extruder
        for (int eid = 0; eid < num_extruders; ++eid) {
            bool clear = true;
            for (int idx = 0; idx <= (int)Slic3r::nvtMaxNozzleVolumeType; ++idx) {
                Slic3r::NozzleVolumeType vol_type = static_cast<Slic3r::NozzleVolumeType>(idx);
                int count = counts[vol_type];
                stat.set_extruder_nozzle_count(eid, vol_type, count, clear);
                clear = false;
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: set extruder=" << eid 
                           << ", volume_type=" << idx << ", count=" << count);
            }
        }
    } else {
        // Standard printer without nozzle rack
        // Map the active nozzle from m_nozzles map
        for (int eid = 0; eid < num_extruders; ++eid) {
            if (nozzle_system->ContainsNozzle(eid)) {
                auto dev_nozzle = nozzle_system->GetNozzle(eid);
                Slic3r::NozzleVolumeType vol_type = Slic3r::nvtStandard;
                if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                    vol_type = Slic3r::nvtHighFlow;
                }
                stat.set_extruder_nozzle_count(eid, vol_type, 1, true);
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: single extruder=" << eid 
                           << ", nozzle found, diameter=" << dev_nozzle.m_diameter 
                           << ", volume_type=" << (int)vol_type);
            } else {
                stat.set_extruder_nozzle_count(eid, Slic3r::nvtStandard, 0, true);
                stat.set_extruder_nozzle_count(eid, Slic3r::nvtHighFlow, 0, false);
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: single extruder=" << eid 
                           << ", nozzle not found, resetting counts to 0");
            }
        }
    }

    VORTEK_LOG(info, "sync_machine_nozzle_inventory_to_preset: completed sync successfully");
}


void set_support_nozzle_rack(Slic3r::MachineObject* obj, bool supported) {
    if (!obj) return;
    auto rack = get_or_create_nozzle_rack(obj);
    if (rack) {
        rack->SetSupported(supported);
    }
}

std::shared_ptr<Slic3r::VortekNozzleRack> get_or_create_nozzle_rack(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_racks.find(obj);
    if (it != s_nozzle_racks.end()) return it->second;
    auto rack = std::make_shared<Slic3r::VortekNozzleRack>(obj->GetNozzleSystem(), obj);
    s_nozzle_racks[obj] = rack;
    return rack;
}

std::shared_ptr<Slic3r::VortekNozzleRack> get_nozzle_rack(const Slic3r::DevNozzleSystem* system) {
    if (!system) return nullptr;
    for (auto const& pair : s_nozzle_racks) {
        if (pair.first->GetNozzleSystem() == system) {
            return pair.second;
        }
    }
    return nullptr;
}

std::set<int> get_ams_binded_extruder_set(const Slic3r::DevAms* ams) {
    if (!ams) return {};
    auto it = s_ams_binded_extruders.find(ams);
    return it != s_ams_binded_extruders.end() ? it->second : std::set<int>{};
}

std::optional<int> get_ams_binded_switcher_pos(const Slic3r::DevAms* ams) {
    if (!ams) return std::nullopt;
    auto it = s_ams_binded_switcher_pos.find(ams);
    return it != s_ams_binded_switcher_pos.end() ? it->second : std::nullopt;
}

std::string get_nozzle_wear(const Slic3r::DevNozzle& nozzle) { return "0"; }
std::string get_nozzle_filament_id(const Slic3r::DevNozzle& nozzle) { return ""; }
std::string get_nozzle_filament_color(const Slic3r::DevNozzle& nozzle) { return ""; }
bool is_nozzle_normal(const Slic3r::DevNozzle& nozzle) { return nozzle.m_nozzle_id != -1; }
int get_nozzle_id(const Slic3r::DevNozzle& nozzle) { return nozzle.m_nozzle_id; }
std::string to_nozzle_flow_string(Slic3r::NozzleFlowType flow_type) {
    return (flow_type == Slic3r::NozzleFlowType::H_FLOW ? "high_flow" : "standard");
}
std::optional<int> get_replace_nozzle_tar(const Slic3r::DevNozzleSystem* system) {
    return std::nullopt;
}

bool is_nozzle_on_rack_helper(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
    auto rack = get_nozzle_rack(system);
    return rack ? rack->IsNozzleOnRack(nozzle_id) : false;
}

std::vector<std::vector<std::vector<float>>> get_full_flush_matrix_helper(const Slic3r::PresetBundle* preset_bundle) {
    std::vector<std::vector<std::vector<float>>> res;
    if (!preset_bundle) return res;
    
    const auto& config = preset_bundle->printers.get_selected_preset().config;
    auto* flush_volumes_matrix_opt = static_cast<const Slic3r::ConfigOptionFloats*>(config.option("flush_volumes_matrix"));
    if (!flush_volumes_matrix_opt) return res;
    
    const auto& values = flush_volumes_matrix_opt->values;
    size_t num_extruders = sqrt(values.size());
    if (num_extruders == 0) return res;
    
    std::vector<std::vector<float>> matrix_2d(num_extruders, std::vector<float>(num_extruders, 0.0f));
    for (size_t r = 0; r < num_extruders; ++r) {
        for (size_t c = 0; c < num_extruders; ++c) {
            size_t idx = r * num_extruders + c;
            if (idx < values.size()) {
                matrix_2d[r][c] = values[idx];
            }
        }
    }
    
    res.resize(4, matrix_2d);
    return res;
}

Slic3r::DevNozzle get_nozzle_by_pos_id(const Slic3r::DevNozzleSystem* system, int pos_id) {
    if (!system) return Slic3r::DevNozzle();
    if (pos_id >= 0x10) {
        auto rack = get_nozzle_rack(system);
        return rack ? rack->GetNozzle(pos_id - 0x10) : Slic3r::DevNozzle();
    } else {
        return system->GetNozzle(pos_id);
    }
}

int get_nozzle_pos_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system) {
    auto rack = get_nozzle_rack(system);
    if (rack && rack->IsNozzleOnRack(nozzle.m_nozzle_id)) {
        return nozzle.m_nozzle_id + 0x10;
    }
    return nozzle.m_nozzle_id;
}

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_or_create_nozzle_mapping(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_mappings.find(obj);
    if (it != s_nozzle_mappings.end()) {
        return it->second;
    }
    auto nm = std::make_shared<Slic3r::VortekNozzleMappingCtrl>(obj);
    s_nozzle_mappings[obj] = nm;
    return nm;
}

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_nozzle_mapping(const Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_mappings.find(obj);
    return it != s_nozzle_mappings.end() ? it->second : nullptr;
}

std::shared_ptr<Slic3r::VortekFilaSwitch> get_or_create_fila_switch(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_fila_switches.find(obj);
    if (it != s_fila_switches.end()) {
        return it->second;
    }
    auto fs = std::make_shared<Slic3r::VortekFilaSwitch>(obj);
    s_fila_switches[obj] = fs;
    return fs;
}

std::shared_ptr<Slic3r::VortekFilaSwitch> get_fila_switch(const Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_fila_switches.find(obj);
    return it != s_fila_switches.end() ? it->second : nullptr;
}

void init_device_mappings(Slic3r::MachineObject* obj) {
    if (!obj) return;
    get_or_create_nozzle_mapping(obj);
    get_or_create_fila_switch(obj);
}

void clear_all_device_mappings(Slic3r::MachineObject* obj) {
    if (!obj) return;
    s_nozzle_racks.erase(obj);
    s_nozzle_mappings.erase(obj);
    s_fila_switches.erase(obj);
}

bool contains_ext_nozzle(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
    if (!system) return false;
    const auto& nozzles = system->GetNozzles();
    auto it = nozzles.find(nozzle_id);
    return it != nozzles.end() && !is_nozzle_empty(it->second);
}

void clear_auto_nozzle_mapping(Slic3r::MachineObject* obj) {
    if (!obj) return;
    auto nm = get_nozzle_mapping(obj);
    if (nm) {
        nm->Clear();
    }
}

static std::map<std::string, std::pair<std::set<int>, std::optional<int>>> s_pending_ams_bindings;

void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json) {
    if (!obj || !filament_json.contains("ams")) return;
    auto fs = get_fila_switch(obj);
    bool fts_installed = fs && fs->IsInstalled();

    s_pending_ams_bindings.clear();

    for (auto& ams_item : filament_json["ams"]) {
        if (!ams_item.contains("id") || !ams_item.contains("extruder_id")) continue;
        
        // Skip parsing if it's not an int (sometimes it's a string, though normally it's an int)
        if (!ams_item["extruder_id"].is_number_integer()) continue;
        
        int ext_id = ams_item["extruder_id"].get<int>();
        std::string ams_id = ams_item["id"].get<std::string>();

        if (ext_id == 0xE) {
            if (fts_installed) {
                // Mutate extruder_id to 0 so the original parser does not erase it
                ams_item["extruder_id"] = MAIN_EXTRUDER_ID;
                
                std::set<int> binded_extruder_set = { MAIN_EXTRUDER_ID, DEPUTY_EXTRUDER_ID };
                std::optional<int> binded_switcher_pos = std::nullopt;
                if (ams_item.contains("info")) {
                    const std::string& info = ams_item["info"].get<std::string>();
                    int bind_switch_in = Slic3r::DevUtil::get_flag_bits(info, 24, 4);
                    if (bind_switch_in == 0) {
                        binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_B;
                    } else if (bind_switch_in == 1) {
                        binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_A;
                    }
                }
                s_pending_ams_bindings[ams_id] = { binded_extruder_set, binded_switcher_pos };
                VORTEK_LOG(info, "preprocess_filament_json: mapped 0xE to MAIN/DEPUTY for ams_id=" << ams_id);
            }
        } else {
            std::set<int> binded_extruder_set = { ext_id };
            s_pending_ams_bindings[ams_id] = { binded_extruder_set, std::nullopt };
        }
    }
}

void apply_pending_ams_bindings(Slic3r::DevFilaSystem* fila_system) {
    if (!fila_system) return;
    const auto& ams_list = fila_system->GetAmsList();
    for (const auto& pending : s_pending_ams_bindings) {
        auto ams_id = pending.first;
        auto it = ams_list.find(ams_id);
        if (it != ams_list.end() && it->second) {
            assign_ams_bindings(it->second, pending.second.first, pending.second.second);
            VORTEK_LOG(info, "apply_pending_ams_bindings: applied pending bindings to ams_id=" << ams_id);
        }
    }
    s_pending_ams_bindings.clear();
}

} // namespace DeviceHooks
} // namespace Vortek
