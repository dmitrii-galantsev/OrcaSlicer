#ifndef VORTEK_PROTOCOL_EXTENSION_H
#define VORTEK_PROTOCOL_EXTENSION_H

#pragma once

#include <string>
#include <mutex>
#include <unordered_map>
#include <ctime>
#include <nlohmann/json.hpp>

namespace Slic3r {
class MachineObject;
class DevNozzleSystem;
}

namespace Vortek {

// VortekProtocolExtension — H2C-only filament name resolution layer.
//
// Resolves filament display names for the Hotends Info view from four sources
// in priority order:
//   1. Active print slot map (mapping[] + AMS + filament_presets)  ← preset used in current print
//      Built from MQTT when gcode_state=RUNNING. Clears on print end.
//      Example: P1f749cd → "ERYONE PA6 Clear - 275" (while printing with that preset)
//   2. Installed presets (PresetCollection::begin()..end())         ← system + user presets
//   3. Default presets  (PresetCollection::lbegin()..begin())       ← generic templates
//   4. Base preset index (user/*/filament/base/*.json)              ← cloud-downloaded base presets
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

    // ── Preset index ──────────────────────────────────────────────────────────
    // Two-tier index built from user/*/filament/ and user/*/filament/base/.
    //
    // m_base_filament_names:
    //   filament_id → stripped display name from base preset files.
    //   Example: "P1f749cd" → "Eryone PA PA6"
    //
    // m_user_preset_overrides:
    //   filament_id → user preset name (highest priority).
    //   Built by matching user preset `inherits` field against base preset names.
    //   Example: "My Eryone" inherits "Eryone PA PA6 @BBL H2C 0.4" →
    //     override["P1f749cd"] = "My Eryone"
    //   Printer suffix stripped before matching to handle "BBL" vs "Bambu Lab".
    //
    // mtime-based invalidation: both user/*/filament/ and user/*/filament/base/
    // directory mtimes are checked. Any change triggers full rebuild.
    // No dependency on UI events — purely data-layer, triggered by query needs.
    // Thread-safe: all access is guarded by m_base_mutex.
    mutable std::mutex                                    m_base_mutex;
    mutable std::time_t                                   m_combined_mtime = 0;  // 0 = never scanned
    mutable std::unordered_map<std::string, std::string>  m_base_filament_names;
    mutable std::unordered_map<std::string, std::string>  m_user_preset_overrides;

    // ── Active print slot map ─────────────────────────────────────────────────
    // Highest-priority name source: maps fila_id → user preset name for the
    // CURRENTLY PRINTING job.
    //
    // Built in preprocess_filament_json when gcode_state=RUNNING:
    //   mapping[i] encodes which physical AMS slot feeds print slot i+1.
    //   Decoding: ams_id = mapping[i] / 256, tray_id = mapping[i] % 256.
    //   AMS tray → tray_info_idx (fila_id).
    //   Preset name from wxGetApp().preset_bundle()->filament_presets[i].
    //
    // Example: mapping[5]=32768 → AMS 128 tray 0 → P1f749cd (Eryone PA6 spool)
    //   filament_presets[5] = "ERYONE PA6 Clear - 275"
    //   → m_print_slot_map["P1f749cd"] = "ERYONE PA6 Clear - 275"
    //
    // Cleared automatically when print finishes (gcode_state ≠ RUNNING).
    // Not thread-safe on its own — only written from the MQTT dispatch thread
    // (same thread as preprocess_filament_json calls).
    // Reference to BBS: BambuStudio push_status mapping[] field
    mutable std::mutex                                    m_print_map_mutex;
    mutable std::unordered_map<std::string, std::string>  m_print_slot_map;    // fila_id → preset name
    mutable std::string                                   m_active_subtask_name; // detect job change

    // Rebuilds m_print_slot_map from the current JSON's mapping[] and AMS data.
    // Called from preprocess_filament_json when gcode_state=RUNNING.
    void build_print_slot_map(const nlohmann::json& filament_json) const;

    // Rebuilds both indexes when any monitored directory mtime changes.
    // Two-pass: base/ presets first, then user/ presets for inherits matching.
    // Called without holding m_base_mutex (acquires it internally).
    void ensure_base_presets_indexed() const;
};


} // namespace Vortek

#endif // VORTEK_PROTOCOL_EXTENSION_H
