#ifndef VORTEK_DEVICE_HOOKS_HPP
#define VORTEK_DEVICE_HOOKS_HPP

#include <memory>
#include <set>
#include <string>
#include <optional>
#include <cstdint>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "DevDefs.h"
#include "DevNozzleSystem.h"
#include "DevFirmware.h"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class PresetBundle;
class MachineObject;
class DevAms;
class DevNozzleSystem;
struct DevNozzle;
class VortekNozzleRack;
class VortekNozzleMappingCtrl;
class VortekFilaSwitch;
class DevFilaSystem;
namespace GUI {
class PartPlate;
}
}

namespace Vortek {
namespace DeviceHooks {

bool is_nozzle_empty(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_unknown(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_info_reliable(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_abnormal(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_diameter_str(const Slic3r::DevNozzle& nozzle);
Slic3r::NozzleFlowType get_nozzle_flow_type(const Slic3r::DevNozzle& nozzle);
int get_logic_extruder_id(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_wear(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_filament_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system = nullptr);
std::string get_nozzle_filament_color(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system = nullptr);
void parse_nozzle_filament(Slic3r::DevNozzleSystem* system, int nozzle_id, const nlohmann::json& njon);
bool is_nozzle_normal(const Slic3r::DevNozzle& nozzle);
int get_nozzle_id(const Slic3r::DevNozzle& nozzle);
std::string to_nozzle_flow_string(Slic3r::NozzleFlowType flow_type);
wxString get_nozzle_type_str(const Slic3r::DevNozzle& nozzle);
wxString get_nozzle_flow_type_str(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_type_string(Slic3r::NozzleType type);
Slic3r::DevFirmwareVersionInfo get_nozzle_firmware_info(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system);
Slic3r::NozzleDiameterType get_nozzle_diameter_type(const Slic3r::DevNozzle& nozzle);
std::optional<int> get_replace_nozzle_tar(const Slic3r::DevNozzleSystem* system);

/**
 * @brief Extract bit flags from a hex string without going out of bounds.
 * @param str The source hex string (optionally starting with 0x).
 * @param start_idx The starting bit index.
 * @param count The number of bits to extract.
 * @return The extracted value as a 32-bit unsigned integer.
 */
uint32_t get_flag_bits_no_border(const std::string& str, int start_idx, int count = 1);

/**
 * @brief Handles AMS extruder bindings when extruder ID is 0xE (V1 protocol).
 * @param obj The MachineObject representing the printer.
 * @param ams_item The JSON object of the AMS from MQTT.
 * @param extruder_id Output physical extruder ID (updated to MAIN_EXTRUDER_ID if FTS is installed).
 * @param binded_extruder_set Output set of binded extruders.
 * @param binded_switcher_pos Output switcher position (0: POS_IN_B, 1: POS_IN_A).
 * @return true if the AMS is valid and parsing should continue; false otherwise.
 */
bool handle_ams_extruder_binding(
    const Slic3r::MachineObject* obj,
    const json& ams_item,
    int& extruder_id,
    std::set<int>& binded_extruder_set,
    std::optional<int>& binded_switcher_pos);

/**
 * @brief Applies the binded extruders and switcher position to the active DevAms object.
 * @param curr_ams The DevAms unit being updated.
 * @param binded_extruder_set The set of binded extruders.
 * @param binded_switcher_pos The optional binded switcher position.
 */
void assign_ams_bindings(
    Slic3r::DevAms* curr_ams,
    const std::set<int>& binded_extruder_set,
    const std::optional<int>& binded_switcher_pos);

std::set<int> get_ams_binded_extruder_set(const Slic3r::DevAms* ams);
std::optional<int> get_ams_binded_switcher_pos(const Slic3r::DevAms* ams);

/**
 * @brief Analyzes nozzle ID and places it either in physical extruder nozzles or in the rack.
 * @param system The DevNozzleSystem manager.
 * @param nozzle_obj The nozzle object to process.
 * @param raw_id The raw nozzle ID containing position and on-rack flag bits.
 */
void process_nozzle_placement(
    Slic3r::DevNozzleSystem* system,
    Slic3r::DevNozzle& nozzle_obj,
    int raw_id);

bool is_h2c_printer(const Slic3r::MachineObject* obj);
void store_wtm_firmware_info(Slic3r::MachineObject* obj, const Slic3r::DevFirmwareVersionInfo& info);
void clear_wtm_firmware_info(Slic3r::MachineObject* obj);

/**
 * @brief Synchronizes nozzle configurations from a connected machine to the current PresetBundle.
 * @param obj The MachineObject representing the printer.
 * @param preset_bundle The active PresetBundle to sync into.
 */
void sync_machine_nozzle_inventory_to_preset(const Slic3r::MachineObject* obj, Slic3r::PresetBundle& preset_bundle);

// Reference to BBS equivalent: DevNozzleSystem::ClearNozzles() in BambuStudio/src/slic3r/GUI/DeviceCore/DevNozzleSystem.cpp:458
void reset_nozzle_system(Slic3r::DevNozzleSystem* system);

void set_support_nozzle_rack(Slic3r::MachineObject* obj, bool supported);

std::shared_ptr<Slic3r::VortekNozzleRack> get_or_create_nozzle_rack(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekNozzleRack> get_nozzle_rack(const Slic3r::DevNozzleSystem* system);
bool is_nozzle_on_rack_helper(const Slic3r::DevNozzleSystem* system, int nozzle_id);
bool contains_ext_nozzle(const Slic3r::DevNozzleSystem* system, int nozzle_id);
std::vector<std::vector<std::vector<float>>> get_full_flush_matrix_helper(const Slic3r::PresetBundle* preset_bundle);
Slic3r::DevNozzle get_nozzle_by_pos_id(const Slic3r::DevNozzleSystem* system, int pos_id);
int get_nozzle_pos_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system);

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_or_create_nozzle_mapping(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_nozzle_mapping(const Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekFilaSwitch> get_or_create_fila_switch(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekFilaSwitch> get_fila_switch(const Slic3r::MachineObject* obj);
void init_device_mappings(Slic3r::MachineObject* obj);
void clear_all_device_mappings(Slic3r::MachineObject* obj);
void clear_auto_nozzle_mapping(Slic3r::MachineObject* obj);
void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json);
void apply_pending_ams_bindings(Slic3r::DevFilaSystem* fila_system);
bool apply_nozzle_mapping_from_device(Slic3r::MachineObject* obj, Slic3r::GUI::PartPlate* plate);

} // namespace DeviceHooks
} // namespace Vortek

#endif // VORTEK_DEVICE_HOOKS_HPP
