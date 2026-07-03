#ifndef VORTEK_PROTOCOL_EXTENSION_H
#define VORTEK_PROTOCOL_EXTENSION_H

#pragma once

#include <string>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace Slic3r {
class MachineObject;
class DevNozzleSystem;
}

namespace Vortek {

// VortekProtocolExtension — H2C-only filament name resolution layer.
//
// Resolves filament display names for the Hotends Info view from three sources
// in priority order:
//   1. Installed presets (PresetCollection::begin()..end())     ← system + user presets
//   2. Default presets  (PresetCollection::lbegin()..begin())   ← generic templates
//   3. Base preset index (user/*/filament/base/*.json)          ← cloud-downloaded base presets
//      Built lazily on first miss, thread-safe, scanned once per session.
//
// Reference to BBS: BambuStudio/src/slic3r/GUI/DeviceCore/DevFilaSystem.cpp
class VortekProtocolExtension {
public:
    static VortekProtocolExtension& get_instance();

    // Resolves a display name for the given filament_id.
    // Searches installed presets, then default presets, then base preset index.
    // Returns empty string if not found.
    std::string resolve_filament_name(const std::string& filament_id) const;

    // Same as above, but also falls back to already-parsed AMS tray data
    // (m_fila_type + sub_brands) available via DevNozzleSystem's FilaSystem.
    // Use from parse_nozzle_filament where MQTT tray JSON is unavailable.
    std::string resolve_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& filament_id) const;

    // Mutates a single tray JSON item to set tray_id_name / tray_sub_brands
    // to a human-readable display name (e.g. "Bambu PLA Basic" instead of "A00-P6").
    void resolve_and_mutate_tray_json(const Slic3r::DevNozzleSystem* nozzle_system, nlohmann::json& tray_item);

    // Entry point for the preprocess_filament hook.
    // Processes all AMS trays and vir_slot entries in the incoming MQTT JSON.
    void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json);

    // Invalidate the base preset index (e.g. after user installs new presets from cloud).
    // The next resolution call will rebuild it lazily.
    void invalidate_base_preset_cache();

    // Looks up filament_id in the base preset index (calls ensure_base_presets_indexed).
    // Public so static helper functions in the .cpp can call via get_instance().
    // Returns display name or empty string.
    std::string lookup_base_preset_name(const std::string& filament_id) const;

private:
    VortekProtocolExtension() = default;
    ~VortekProtocolExtension() = default;
    VortekProtocolExtension(const VortekProtocolExtension&) = delete;
    VortekProtocolExtension& operator=(const VortekProtocolExtension&) = delete;

    // ── Base preset index ─────────────────────────────────────────────────────
    // Populated lazily from user/*/filament/base/*.json files.
    // Key:   filament_id (e.g. "P1f749cd")
    // Value: stripped display name (e.g. "Eryone PA PA6")
    //
    // Thread-safe: all access is guarded by m_base_mutex.
    // Not a static variable: lives in the singleton instance.
    mutable std::mutex                           m_base_mutex;
    mutable bool                                 m_base_indexed = false;
    mutable std::unordered_map<std::string, std::string> m_base_filament_names;

    // Scans user/*/filament/base/*.json exactly once per session.
    // Must be called with m_base_mutex NOT held (it acquires the lock internally).
    void ensure_base_presets_indexed() const;
};

} // namespace Vortek

#endif // VORTEK_PROTOCOL_EXTENSION_H
