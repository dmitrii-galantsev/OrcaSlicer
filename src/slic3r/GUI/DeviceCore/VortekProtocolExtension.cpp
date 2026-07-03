#include "VortekProtocolExtension.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/VortekDeviceHooks.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/VortekLog.hpp"
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

void VortekProtocolExtension::ensure_base_presets_indexed() const {
    std::lock_guard<std::mutex> lock(m_base_mutex);
    if (m_base_indexed) return;
    m_base_indexed = true;

    // Scan user/*/filament/base/*.json for cloud-downloaded base presets.
    // These presets have filament_id set (e.g. "P1f749cd") but live outside
    // the standard PresetCollection iteration range.
    // Reference to BBS: BambuStudio user data layout (user/<uid>/filament/base/)
    namespace fs = boost::filesystem;
    try {
        const fs::path user_dir = fs::path(Slic3r::data_dir()) / "user";
        if (!fs::exists(user_dir) || !fs::is_directory(user_dir)) return;

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
                    if (fid.empty()) continue;
                    // Skip if already found in a higher-priority pass
                    if (m_base_filament_names.count(fid)) continue;

                    std::string name = j.contains("name") ? j["name"].get<std::string>() : "";
                    if (name.empty()) name = entry.path().stem().string();
                    std::string display = strip_printer_suffix(name);
                    if (!display.empty()) {
                        m_base_filament_names[fid] = display;
                        VORTEK_LOG(warn, "VortekProtocolExtension: indexed base preset: id=" << fid << ", name=" << display);
                    }
                } catch (...) {
                    // Skip malformed or unreadable JSON files silently
                }
            }
        }
    } catch (...) {
        // Skip if data dir not accessible (sandboxed or missing)
    }
    VORTEK_LOG(warn, "VortekProtocolExtension: base preset index built: " << m_base_filament_names.size() << " entries");
}

std::string VortekProtocolExtension::lookup_base_preset_name(const std::string& filament_id) const {
    ensure_base_presets_indexed();
    std::lock_guard<std::mutex> lock(m_base_mutex);
    auto it = m_base_filament_names.find(filament_id);
    return (it != m_base_filament_names.end()) ? it->second : "";
}

void VortekProtocolExtension::invalidate_base_preset_cache() {
    std::lock_guard<std::mutex> lock(m_base_mutex);
    m_base_indexed = false;
    m_base_filament_names.clear();
    VORTEK_LOG(warn, "VortekProtocolExtension: base preset cache invalidated");
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
