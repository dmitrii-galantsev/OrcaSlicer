#include "VortekProtocolExtension.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/VortekDeviceHooks.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/VortekLog.hpp"
#include <ctime>
#include <fstream>
#include <boost/filesystem.hpp>

namespace Vortek {

// Strip " @..." printer suffix from preset name.
// "Bambu PLA Basic @BBL H2C 0.4 nozzle" → "Bambu PLA Basic"
// Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp (name/inherits structure)
static std::string strip_printer_suffix(const std::string& name) {
    const auto at_pos = name.find(" @");
    return (at_pos != std::string::npos) ? name.substr(0, at_pos) : name;
}

VortekProtocolExtension& VortekProtocolExtension::get_instance() {
    static VortekProtocolExtension instance;
    return instance;
}

// ── Base preset index ────────────────────────────────────────────────────────

// Returns the max mtime across all user/*/filament/ and user/*/filament/base/ directories.
// Monitors both to detect: new base presets (cloud sync) AND user preset create/delete.
// Cheap: only stats directories, not individual files.
// Returns 0 if not accessible or not found.
// Reference to BBS: BambuStudio user data layout (user/<uid>/filament/)
static std::time_t get_combined_dirs_mtime() {
    namespace fs = boost::filesystem;
    std::time_t max_mtime = 0;
    try {
        const fs::path user_dir = fs::path(Slic3r::data_dir()) / "user";
        if (!fs::exists(user_dir) || !fs::is_directory(user_dir)) return 0;
        for (const auto& user_entry : fs::directory_iterator(user_dir)) {
            if (!fs::is_directory(user_entry)) continue;
            const fs::path fila_dir = user_entry.path() / "filament";
            if (fs::exists(fila_dir) && fs::is_directory(fila_dir)) {
                const std::time_t t = fs::last_write_time(fila_dir);
                if (t > max_mtime) max_mtime = t;
            }
            const fs::path base_dir = fila_dir / "base";
            if (fs::exists(base_dir) && fs::is_directory(base_dir)) {
                const std::time_t t = fs::last_write_time(base_dir);
                if (t > max_mtime) max_mtime = t;
            }
        }
    } catch (...) {}
    return max_mtime;
}

void VortekProtocolExtension::ensure_base_presets_indexed() const {
    // Combined mtime check: monitors user/*/filament/ and user/*/filament/base/.
    // Detects: cloud sync (new base presets) AND user preset create/delete.
    // Rebuild triggers two-pass scan: base presets first, user presets second.
    // No UI dependency — pure data-layer, triggered by query needs.
    // Reference to BBS: BambuStudio user data layout (user/<uid>/filament/)
    const std::time_t current_mtime = get_combined_dirs_mtime();
    {
        std::lock_guard<std::mutex> lock(m_base_mutex);
        if (current_mtime != 0 && current_mtime == m_combined_mtime) {
            return;  // directories unchanged — indexes still valid
        }
        const bool is_first = (m_combined_mtime == 0);
        VORTEK_LOG(warn, "VortekProtocolExtension: preset index "
            << (is_first ? "initial scan" : "rebuild (dir mtime changed)")
            << ": stored_mtime=" << m_combined_mtime
            << ", current_mtime=" << current_mtime);
        m_combined_mtime = current_mtime;
        m_base_filament_names.clear();
        m_user_preset_overrides.clear();
    }

    // Two-pass scan using local accumulators (no lock held during file I/O).
    // Pass 1 must complete before Pass 2 (reverse map needed for inherits lookup).
    namespace fs = boost::filesystem;
    std::unordered_map<std::string, std::string> local_fid_names;    // fid → base display name
    std::unordered_map<std::string, std::string> local_rev_names;    // stripped_name → fid (reverse)
    std::unordered_map<std::string, std::string> local_user_override; // fid → user preset name

    const fs::path user_dir = fs::path(Slic3r::data_dir()) / "user";

    // ── Pass 1: scan user/*/filament/base/*.json ──────────────────────────────
    // Build fid→name (base index) and stripped_name→fid (reverse, for Pass 2).
    // Reference to BBS: BambuStudio user data layout (user/<uid>/filament/base/)
    try {
        if (fs::exists(user_dir) && fs::is_directory(user_dir)) {
            for (const auto& user_entry : fs::directory_iterator(user_dir)) {
                if (!fs::is_directory(user_entry)) continue;
                const fs::path base_dir = user_entry.path() / "filament" / "base";
                if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) continue;
                for (const auto& entry : fs::directory_iterator(base_dir)) {
                    if (entry.path().extension() != ".json") continue;
                    try {
                        std::ifstream f(entry.path().string());
                        if (!f.is_open()) continue;
                        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
                        if (j.is_discarded() || !j.contains("filament_id")) continue;
                        std::string fid = j["filament_id"].get<std::string>();
                        if (fid.empty() || fid == "null") continue;
                        if (local_fid_names.count(fid)) continue;  // first-wins
                        std::string name = j.contains("name") ? j["name"].get<std::string>() : "";
                        if (name.empty()) name = entry.path().stem().string();
                        const std::string display = strip_printer_suffix(name);
                        if (display.empty()) continue;
                        local_fid_names[fid] = display;
                        local_rev_names[display] = fid;  // reverse: stripped name → fid
                        VORTEK_LOG(warn, "VortekProtocolExtension: indexed base preset: id=" << fid << ", name=" << display);
                    } catch (...) {}
                }
            }
        }
    } catch (...) {}

    // ── Pass 2: scan user/*/filament/*.json for inherits-chain matching ────────
    // For each user preset (filament_id empty), check if its `inherits` field maps
    // to a known base preset via reverse lookup.
    // Printer suffix stripped before matching: "BBL H2C" == "Bambu Lab H2C".
    // Priority: most recently created user preset wins per filament_id (first-wins).
    // Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp (inherits field structure)
    try {
        if (fs::exists(user_dir) && fs::is_directory(user_dir)) {
            for (const auto& user_entry : fs::directory_iterator(user_dir)) {
                if (!fs::is_directory(user_entry)) continue;
                const fs::path fila_dir = user_entry.path() / "filament";
                if (!fs::exists(fila_dir) || !fs::is_directory(fila_dir)) continue;
                for (const auto& entry : fs::directory_iterator(fila_dir)) {
                    if (!fs::is_regular_file(entry) || entry.path().extension() != ".json") continue;
                    try {
                        std::ifstream f(entry.path().string());
                        if (!f.is_open()) continue;
                        nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
                        if (j.is_discarded() || !j.contains("inherits")) continue;
                        // Skip if preset already has a real filament_id (handled by PresetCollection)
                        if (j.contains("filament_id")) {
                            const auto& fid_val = j["filament_id"];
                            const std::string fid_check = fid_val.is_string() ? fid_val.get<std::string>() : "";
                            if (!fid_check.empty() && fid_check != "null") continue;
                        }
                        const std::string inherits_val = j["inherits"].get<std::string>();
                        if (inherits_val.empty()) continue;
                        // Strip printer suffix before lookup: handles "BBL" vs "Bambu Lab" mismatch
                        const std::string inherits_stripped = strip_printer_suffix(inherits_val);
                        const auto rev_it = local_rev_names.find(inherits_stripped);
                        if (rev_it == local_rev_names.end()) continue;  // parent not a known base preset
                        const std::string& fid = rev_it->second;
                        if (local_user_override.count(fid)) continue;  // first-wins
                        std::string user_name = j.contains("name") ? j["name"].get<std::string>() : "";
                        if (user_name.empty()) user_name = entry.path().stem().string();
                        const std::string user_display = strip_printer_suffix(user_name);
                        if (user_display.empty()) continue;
                        local_user_override[fid] = user_display;
                        VORTEK_LOG(warn, "VortekProtocolExtension: user preset override: id=" << fid
                            << ", preset=" << user_display
                            << " (inherits: " << inherits_stripped << ")");
                    } catch (...) {}
                }
            }
        }
    } catch (...) {}

    // Atomically commit both indexes under lock.
    {
        std::lock_guard<std::mutex> lock(m_base_mutex);
        m_base_filament_names  = std::move(local_fid_names);
        m_user_preset_overrides = std::move(local_user_override);
        VORTEK_LOG(warn, "VortekProtocolExtension: preset index ready: "
            << m_base_filament_names.size() << " base entries, "
            << m_user_preset_overrides.size() << " user overrides");
    }
}

std::string VortekProtocolExtension::lookup_base_preset_name(const std::string& filament_id) const {
    ensure_base_presets_indexed();
    std::lock_guard<std::mutex> lock(m_base_mutex);
    // Priority: user preset override (custom name) > base preset name
    auto user_it = m_user_preset_overrides.find(filament_id);
    if (user_it != m_user_preset_overrides.end()) return user_it->second;
    auto base_it = m_base_filament_names.find(filament_id);
    return (base_it != m_base_filament_names.end()) ? base_it->second : "";
}

void VortekProtocolExtension::invalidate_base_preset_cache() {
    std::lock_guard<std::mutex> lock(m_base_mutex);
    m_combined_mtime = 0;  // reset to 0 = "never scanned" → forces full rescan on next query
    m_base_filament_names.clear();
    m_user_preset_overrides.clear();
    VORTEK_LOG(warn, "VortekProtocolExtension: preset cache invalidated");
}

// ── Active print slot map ───────────────────────────────────────────────────

// Builds m_print_slot_map from the current MQTT push_status JSON.
// Decodes mapping[i] = ams_id*256 + tray_id (0xFFFF = unassigned).
// Cross-references AMS tray → fila_id with filament_presets[i] from PresetBundle.
// Called from preprocess_filament_json when gcode_state=RUNNING.
// Reference to BBS: BambuStudio push_status: mapping[], ams.ams[].tray[].tray_info_idx
void VortekProtocolExtension::build_print_slot_map(const nlohmann::json& json) const {
    if (!Slic3r::GUI::wxGetApp().preset_bundle) return;

    // Extract gcode_state and subtask name
    const std::string gcode_state = (json.contains("gcode_state") && json["gcode_state"].is_string())
        ? json["gcode_state"].get<std::string>() : "";
    const std::string subtask = (json.contains("subtask_name") && json["subtask_name"].is_string())
        ? json["subtask_name"].get<std::string>() : "";

    // Clear map when not actively printing
    if (gcode_state != "RUNNING") {
        std::lock_guard<std::mutex> lock(m_print_map_mutex);
        if (!m_print_slot_map.empty()) {
            m_print_slot_map.clear();
            m_active_subtask_name.clear();
            VORTEK_LOG(warn, "VortekProtocolExtension: print slot map cleared (state=" << gcode_state << ")");
        }
        return;
    }

    // Skip rebuild if same job already mapped
    {
        std::lock_guard<std::mutex> lock(m_print_map_mutex);
        if (!m_print_slot_map.empty() && subtask == m_active_subtask_name) return;
    }

    // Parse mapping[]
    if (!json.contains("mapping") || !json["mapping"].is_array()) return;
    const auto& mapping_arr = json["mapping"];

    // Build AMS tray index: ams_id (int) → ordered list of tray_info_idx strings
    // tray position in array = tray id (0,1,2,3)
    // Reference to BBS: BambuStudio MachineObject::parse_json AMS tray iteration
    std::unordered_map<int, std::vector<std::string>> ams_tray_idx;
    if (json.contains("ams") && json["ams"].contains("ams") && json["ams"]["ams"].is_array()) {
        for (const auto& ams_item : json["ams"]["ams"]) {
            if (!ams_item.contains("id") || !ams_item.contains("tray")) continue;
            int ams_id_num = 0;
            try { ams_id_num = std::stoi(ams_item["id"].get<std::string>()); } catch (...) { continue; }
            const auto& tray_arr = ams_item["tray"];
            if (!tray_arr.is_array()) continue;
            std::vector<std::string> trays;
            for (const auto& tray : tray_arr) {
                const std::string fid = (tray.contains("tray_info_idx") && tray["tray_info_idx"].is_string())
                    ? tray["tray_info_idx"].get<std::string>() : "";
                trays.push_back(fid);
            }
            ams_tray_idx[ams_id_num] = std::move(trays);
        }
    }

    // Cross-reference mapping[] with filament_presets to build fila_id → preset name
    const auto& filament_presets = Slic3r::GUI::wxGetApp().preset_bundle->filament_presets;
    std::unordered_map<std::string, std::string> new_map;

    for (size_t i = 0; i < mapping_arr.size(); ++i) {
        if (!mapping_arr[i].is_number_integer()) continue;
        const int mapping_val = mapping_arr[i].get<int>();
        if (mapping_val < 0 || mapping_val == 0xFFFF) continue;   // unassigned

        const int ams_id  = mapping_val / 256;
        const int tray_id = mapping_val % 256;

        auto ams_it = ams_tray_idx.find(ams_id);
        if (ams_it == ams_tray_idx.end()) continue;
        const auto& trays = ams_it->second;
        if (tray_id < 0 || static_cast<size_t>(tray_id) >= trays.size()) continue;

        const std::string& fila_id = trays[static_cast<size_t>(tray_id)];
        if (fila_id.empty()) continue;

        if (i >= filament_presets.size()) continue;
        const std::string preset_name = strip_printer_suffix(filament_presets[i]);
        if (preset_name.empty()) continue;

        // Only store if preset name differs from base — skip generic names that add no info
        new_map[fila_id] = preset_name;
        VORTEK_LOG(warn, "VortekProtocolExtension: print slot map: slot=" << (i + 1)
            << " mapping=" << mapping_val
            << " ams=" << ams_id << " tray=" << tray_id
            << " fila_id=" << fila_id
            << " preset=" << preset_name);
    }

    {
        std::lock_guard<std::mutex> lock(m_print_map_mutex);
        m_print_slot_map     = std::move(new_map);
        m_active_subtask_name = subtask;
        VORTEK_LOG(warn, "VortekProtocolExtension: print slot map ready: "
            << m_print_slot_map.size() << " entries, job=" << subtask);
    }
}

// ── Preset name lookup helpers ────────────────────────────────────────────────

// Look up a clean display name from the installed preset collection by filament_id.
// filament_id is inherited from the base preset to all child presets.
// Returns stripped name (e.g. "Bambu PLA Basic") or "" if not found.
// Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp#L1698 (filament_id inherited from parent)
// Three-pass search:
//   Pass 1: begin()..end()              — user/system visible presets (highest priority)
//   Pass 2: lbegin()..begin()           — generic default presets
//   Pass 3: m_base_filament_names index — cloud-downloaded base/ presets (lazily scanned)
static std::string preset_name_by_filament_id(const std::string& filament_id) {
    if (!Slic3r::GUI::wxGetApp().preset_bundle) return "";
    auto& bundle = *Slic3r::GUI::wxGetApp().preset_bundle;  // non-const: lbegin() is not const
    // Pass 1: user/visible presets
    for (auto it = bundle.filaments.begin(); it != bundle.filaments.end(); ++it) {
        if (it->filament_id == filament_id) {
            return strip_printer_suffix(it->name);
        }
    }
    // Pass 2: default/generic presets (before m_num_default_presets cut)
    // Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp (lbegin)
    for (auto it = bundle.filaments.lbegin(); it != bundle.filaments.begin(); ++it) {
        if (it->filament_id == filament_id) {
            return strip_printer_suffix(it->name);
        }
    }
    // Pass 3: base preset index (user/*/filament/base/*.json)
    // Cloud-downloaded base presets that are NOT in the PresetCollection iteration range.
    // Scanned lazily on first call, cached for the session (invalidated on preset sync).
    return VortekProtocolExtension::get_instance().lookup_base_preset_name(filament_id);
}


std::string VortekProtocolExtension::resolve_filament_name(const std::string& filament_id) const {
    // Reference to BBS: BambuStudio/src/slic3r/GUI/DeviceCore/DevFilaSystem.cpp#L789
    if (filament_id.empty() || !Slic3r::GUI::wxGetApp().preset_bundle) {
        return "";
    }

    // Priority 1 (highest): Active print slot map.
    // Shows the user's configured preset name for the CURRENT PRINT.
    // e.g. P1f749cd → "ERYONE PA6 Clear - 275" while printing with that preset.
    // Built from MQTT mapping[] in preprocess_filament_json. Cleared when print ends.
    {
        std::lock_guard<std::mutex> lock(m_print_map_mutex);
        auto it = m_print_slot_map.find(filament_id);
        if (it != m_print_slot_map.end() && !it->second.empty()) return it->second;
    }
    // For all filaments: try preset lookup first (inherited filament_id works for all)
    std::string name = preset_name_by_filament_id(filament_id);
    if (!name.empty()) return name;

    // For custom/third-party filaments ("P*") not found in standard collection:
    // use get_filament_by_filament_id which searches all installed user presets.
    // NOTE: filament_name field = alias (may be internal code for GF*), so prefer
    // the iterator name result instead.
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.hpp#L256
    auto& bundle = *Slic3r::GUI::wxGetApp().preset_bundle;
    auto opt = bundle.get_filament_by_filament_id(filament_id);
    if (opt.has_value() && !opt->filament_name.empty()) {
        // filament_name is the alias which for custom presets IS the display name
        // (e.g. "Eryone PA PA6"), but for GF* it would be a color code ("A00-P6").
        // Since GF* was handled above via preset_name_by_filament_id, only custom presets reach here.
        return opt->filament_name;
    }

    return "";
}

std::string VortekProtocolExtension::resolve_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& filament_id) const {
    // 1. Try preset-based resolution
    std::string name = resolve_filament_name(filament_id);
    if (!name.empty()) return name;

    // 2. Fallback: search already-parsed AMS trays in DevFilaSystem for matching setting_id.
    // Reference to BBS: BambuStudio/src/slic3r/GUI/DeviceCore/DevFilaSystem.cpp (tray setting_id usage)
    if (!system || !system->GetOwner() || !system->GetOwner()->GetFilaSystem()) return "";

    const auto& ams_list = system->GetOwner()->GetFilaSystem()->GetAmsList();
    for (const auto& [ams_id, ams] : ams_list) {
        if (!ams) continue;
        for (const auto& [tray_id, tray] : ams->GetTrays()) {
            if (!tray) continue;
            if (tray->setting_id != filament_id) continue;

            const std::string& f_type    = tray->m_fila_type;
            const std::string& sub_brand = tray->sub_brands;
            if (f_type.empty()) continue;

            if (!sub_brand.empty() && sub_brand != f_type) {
                name = f_type + " (" + sub_brand + ")";
            } else {
                name = f_type;
            }
            VORTEK_LOG(warn, "VortekProtocolExtension::resolve_filament_name(system): FilaSystem fallback: id=" << filament_id << ", name=" << name);
            return name;
        }
    }
    return "";
}

void VortekProtocolExtension::resolve_and_mutate_tray_json(const Slic3r::DevNozzleSystem* nozzle_system, nlohmann::json& tray_item) {
    if (!nozzle_system || !tray_item.contains("tray_info_idx")) return;

    std::string f_id = tray_item["tray_info_idx"].get<std::string>();
    if (f_id.empty()) return;

    std::string t_type     = tray_item.contains("tray_type")      ? tray_item["tray_type"].get<std::string>()      : "";
    std::string sub_brands = tray_item.contains("tray_sub_brands") ? tray_item["tray_sub_brands"].get<std::string>() : "";
    std::string tray_id_nm = tray_item.contains("tray_id_name")    ? tray_item["tray_id_name"].get<std::string>()    : "";

    std::string display_name;

    if (f_id.size() >= 2 && f_id[0] == 'G' && f_id[1] == 'F') {
        // ── Standard Bambu system filaments ("GF*") ──────────────────────────────
        // tray_id_name is a spool color variant code ("A00-P6", "G50-P7"), NOT display name.
        // 1. Try preset collection: filament_id is inherited from base preset.
        //    "Bambu PLA Basic @BBL H2C 0.4 nozzle" → strip suffix → "Bambu PLA Basic"
        //    Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp#L1698
        display_name = preset_name_by_filament_id(f_id);

        // 2. Fallback to tray_sub_brands from MQTT ("PLA Basic", "PETG-CF" etc.)
        if (display_name.empty() && !sub_brands.empty()) {
            display_name = sub_brands;
        }
        // 3. Last resort: tray_type ("PLA", "PETG-CF")
        if (display_name.empty() && !t_type.empty()) {
            display_name = t_type;
        }
    } else {
        // ── Custom / third-party filaments ("P*") ────────────────────────────────
        // 1. tray_id_name — only if it looks like a real name (not a color variant code).
        //    Color codes match pattern: 3+ uppercase letters/digits, dash, letter+digits (e.g. "A00-P6").
        //    Real names contain spaces or known keywords. Simple heuristic: accept if has a space.
        if (!tray_id_nm.empty() && tray_id_nm.find(' ') != std::string::npos) {
            display_name = tray_id_nm;
        }
        // 2. Preset lookup via filament_id or get_filament_by_filament_id
        if (display_name.empty()) {
            display_name = resolve_filament_name(f_id);
        }
        // 3. tray_sub_brands / tray_type fallback
        if (display_name.empty() && !sub_brands.empty()) {
            display_name = sub_brands;
        }
        if (display_name.empty() && !t_type.empty()) {
            display_name = t_type;
        }
    }

    if (!display_name.empty()) {
        tray_item["tray_id_name"]    = display_name;
        tray_item["tray_sub_brands"] = display_name;
        DeviceHooks::set_custom_filament_name(nozzle_system, f_id, display_name);
        VORTEK_LOG(warn, "VortekProtocolExtension: cached filament: id=" << f_id << ", name=" << display_name << " [sub_brands=" << sub_brands << ", tray_id_name=" << tray_id_nm << "]");
    }
}

void VortekProtocolExtension::preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json) {
    // Hook isolation: verify that the printer supports Vortek/H2C configuration
    if (!obj || !filament_json.contains("ams") || !DeviceHooks::is_h2c_printer(obj)) return;

    // 0. Build/update active print slot map (fila_id → user preset name for current print).
    //    Priority-1 source for resolve_filament_name: shows "ERYONE PA6 Clear - 275" instead of
    //    "Eryone PA PA6" while that preset is actively printing.
    //    No-op when gcode_state ≠ RUNNING.
    build_print_slot_map(filament_json);

    // 1. Process AMS trays
    if (filament_json["ams"].contains("ams") && filament_json["ams"]["ams"].is_array()) {
        for (auto& ams_item : filament_json["ams"]["ams"]) {
            if (ams_item.contains("tray") && ams_item["tray"].is_array()) {
                for (auto& tray_item : ams_item["tray"]) {
                    resolve_and_mutate_tray_json(obj->GetNozzleSystem(), tray_item);
                }
            }
        }
    }

    // 2. Process Virtual/External trays
    if (filament_json.contains("vir_slot") && filament_json["vir_slot"].is_array()) {
        for (auto& v_slot : filament_json["vir_slot"]) {
            resolve_and_mutate_tray_json(obj->GetNozzleSystem(), v_slot);
        }
    }
}

} // namespace Vortek
