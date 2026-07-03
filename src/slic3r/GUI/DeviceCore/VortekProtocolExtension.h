#ifndef VORTEK_PROTOCOL_EXTENSION_H
#define VORTEK_PROTOCOL_EXTENSION_H

#pragma once

#include <string>
#include <set>
#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r {
class MachineObject;
class DevNozzleSystem;
}

namespace Vortek {

class VortekProtocolExtension {
public:
    static VortekProtocolExtension& get_instance();

    // Resolves custom name for a given filament ID by searching all loaded presets in PresetBundle.
    // Returns empty string if not found.
    std::string resolve_filament_name(const std::string& filament_id) const;

    // Same as above, but also falls back to already-parsed AMS tray data (m_fila_type + sub_brands)
    // available in the DevNozzleSystem owner's FilaSystem when the preset is absent on disk.
    // Use this variant from parse_nozzle_filament where tray_type is not available in the JSON.
    std::string resolve_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& filament_id) const;

    // Mutates a single tray JSON object by mapping tray_info_idx to tray_id_name / tray_sub_brands
    void resolve_and_mutate_tray_json(const Slic3r::DevNozzleSystem* nozzle_system, nlohmann::json& tray_item);

    // Preprocesses incoming MQTT JSON data from the printer
    void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json);

private:
    VortekProtocolExtension() = default;
    ~VortekProtocolExtension() = default;
    VortekProtocolExtension(const VortekProtocolExtension&) = delete;
    VortekProtocolExtension& operator=(const VortekProtocolExtension&) = delete;
};

} // namespace Vortek

#endif // VORTEK_PROTOCOL_EXTENSION_H
