#include "VortekMultiNozzle.hpp"
#include "VortekLog.hpp"
#include <sstream>
#include <algorithm>

namespace Slic3r {
namespace MultiNozzleUtils {

// Helper to format nozzle diameter to string
static std::string format_diameter_to_str(double diameter) {
    char buf[64];
    sprintf(buf, "%.2f", diameter);
    std::string str(buf);
    while (!str.empty() && str.back() == '0') {
        str.pop_back();
    }
    if (!str.empty() && str.back() == '.') {
        str.pop_back();
    }
    return str;
}

std::string NozzleInfo::serialize() const {
    std::stringstream ss;
    ss << diameter << ":" << static_cast<int>(volume_type) << ":" << extruder_id << ":" << group_id;
    return ss.str();
}

std::string NozzleGroupInfo::serialize() const {
    std::stringstream ss;
    ss << diameter << ":" << static_cast<int>(volume_type) << ":" << extruder_id << ":" << nozzle_count;
    return ss.str();
}

std::optional<NozzleGroupInfo> NozzleGroupInfo::deserialize(const std::string& str) {
    std::vector<std::string> tokens;
    std::string token;
    std::stringstream ss(str);
    while (std::getline(ss, token, ':')) {
        tokens.push_back(token);
    }
    if (tokens.size() < 4) {
        return std::nullopt;
    }
    try {
        std::string diameter = tokens[0];
        NozzleVolumeType volume_type = static_cast<NozzleVolumeType>(std::stoi(tokens[1]));
        int extruder_id = std::stoi(tokens[2]);
        int nozzle_count = std::stoi(tokens[3]);
        return NozzleGroupInfo(diameter, volume_type, extruder_id, nozzle_count);
    } catch (...) {
        return std::nullopt;
    }
}

// ==================== LayeredNozzleGroupResult Implementation ====================

static bool has_filament_mapped_to_multiple_nozzles(
    const std::vector<std::vector<int>>& layer_filament_nozzle_maps,
    const std::vector<unsigned int>& used_filaments) {
    if (layer_filament_nozzle_maps.empty() || used_filaments.empty()) {
        return false;
    }
    for (auto filament_id_u : used_filaments) {
        int filament_id = static_cast<int>(filament_id_u);
        std::set<int> nozzle_ids;
        for (const auto& map : layer_filament_nozzle_maps) {
            if (filament_id < 0 || filament_id >= static_cast<int>(map.size())) {
                continue;
            }
            int nozzle_id = map[filament_id];
            if (nozzle_id < 0) {
                continue;
            }
            nozzle_ids.insert(nozzle_id);
            if (nozzle_ids.size() > 1) {
                return true;
            }
        }
    }
    return false;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<int>& filament_nozzle_map,
    const std::vector<NozzleInfo>& nozzle_list,
    const std::vector<unsigned int>& used_filaments) {
    VORTEK_LOG(debug, "LayeredNozzleGroupResult::create (simple): nozzles = " << nozzle_list.size() << ", filaments = " << used_filaments.size());
    if (filament_nozzle_map.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }
    LayeredNozzleGroupResult result(false);
    result._default_filament_nozzle_map = filament_nozzle_map;
    result._nozzle_list = nozzle_list;
    result._used_filaments = used_filaments;
    return result;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<std::vector<int>>& layer_filament_nozzle_maps,
    const std::vector<NozzleInfo>& nozzle_list,
    const std::vector<unsigned int>& used_filaments,
    const std::vector<std::vector<unsigned int>>& layer_filament_sequences) {
    VORTEK_LOG(debug, "LayeredNozzleGroupResult::create (layered): nozzles = " << nozzle_list.size() << ", layers = " << layer_filament_nozzle_maps.size());
    if (layer_filament_nozzle_maps.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }
    bool support_dynamic_nozzle_map = has_filament_mapped_to_multiple_nozzles(layer_filament_nozzle_maps, used_filaments);
    LayeredNozzleGroupResult result(support_dynamic_nozzle_map);
    result._layer_filament_nozzle_maps = layer_filament_nozzle_maps;
    result._layer_filament_sequences = layer_filament_sequences;
    result._nozzle_list = nozzle_list;
    result._used_filaments = used_filaments;
    if (!layer_filament_nozzle_maps.empty()) {
        result._default_filament_nozzle_map = layer_filament_nozzle_maps[0];
    }
    return result;
}

std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create(
    const std::vector<unsigned int>& used_filaments,
    const std::vector<int>& filament_map,
    const std::vector<int>& filament_volume_map,
    const std::vector<int>& filament_nozzle_map,
    const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
    float diameter) {
    std::vector<NozzleGroupInfo> nozzle_groups;
    for (size_t extruder_id = 0; extruder_id < nozzle_count.size(); ++extruder_id) {
        for (auto elem : nozzle_count[extruder_id]) {
            NozzleGroupInfo group_info;
            group_info.diameter = format_diameter_to_str(diameter);
            group_info.volume_type = elem.first;
            group_info.nozzle_count = elem.second;
            group_info.extruder_id = static_cast<int>(extruder_id);
            nozzle_groups.emplace_back(group_info);
        }
    }
    auto nozzle_list = build_nozzle_list(nozzle_groups);
    std::vector<bool> used_nozzle(nozzle_list.size(), false);
    std::map<int, int> input_nozzle_id_to_output;
    std::vector<int> output_nozzle_map(filament_nozzle_map.size(), 0);

    for (auto filament_idx : used_filaments) {
        if (filament_idx >= filament_volume_map.size() || filament_idx >= filament_map.size() || filament_idx >= filament_nozzle_map.size()) {
            return std::nullopt;
        }
        NozzleVolumeType req_type = NozzleVolumeType(filament_volume_map[filament_idx]);
        int req_extruder = filament_map[filament_idx];
        int input_nozzle_idx = filament_nozzle_map[filament_idx];

        if (input_nozzle_id_to_output.find(input_nozzle_idx) != input_nozzle_id_to_output.end()) {
            output_nozzle_map[filament_idx] = input_nozzle_id_to_output[input_nozzle_idx];
            continue;
        }

        int output_nozzle_idx = -1;
        for (size_t nozzle_idx = 0; nozzle_idx < nozzle_list.size(); ++nozzle_idx) {
            if (used_nozzle[nozzle_idx]) continue;
            auto& nozzle_info = nozzle_list[nozzle_idx];
            if (!(nozzle_info.extruder_id == req_extruder && nozzle_info.volume_type == req_type)) continue;

            output_nozzle_idx = static_cast<int>(nozzle_idx);
            input_nozzle_id_to_output[input_nozzle_idx] = output_nozzle_idx;
            used_nozzle[nozzle_idx] = true;
            break;
        }

        if (output_nozzle_idx == -1) {
            return std::nullopt;
        }
        output_nozzle_map[filament_idx] = output_nozzle_idx;
    }
    return create(output_nozzle_map, nozzle_list, used_filaments);
}

bool LayeredNozzleGroupResult::are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id) const {
    auto nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    auto nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);
    if (!nozzle_info1 || !nozzle_info2) return false;
    return nozzle_info1->extruder_id == nozzle_info2->extruder_id;
}

bool LayeredNozzleGroupResult::are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id) const {
    auto nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    auto nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);
    if (!nozzle_info1 || !nozzle_info2) return false;
    return nozzle_info1->group_id == nozzle_info2->group_id;
}

int LayeredNozzleGroupResult::get_extruder_count() const {
    std::set<int> extruder_ids;
    for (const auto& nozzle : _nozzle_list) {
        extruder_ids.insert(nozzle.extruder_id);
    }
    return static_cast<int>(extruder_ids.size());
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id) const {
    return get_used_nozzles_in_extruder(target_extruder_id, -1);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const {
    std::set<int> nozzle_ids;
    std::vector<NozzleInfo> result;
    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);

    for (unsigned int filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) {
                if (target_extruder_id == -1 || nozzle_opt->extruder_id == target_extruder_id) {
                    nozzle_ids.insert(nozzle_opt->group_id);
                }
            }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto &nozzle : nozzles) {
                if (target_extruder_id == -1 || nozzle.extruder_id == target_extruder_id) {
                    nozzle_ids.insert(nozzle.group_id);
                }
            }
        }
    }
    for (int nozzle_id : nozzle_ids) {
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            result.push_back(_nozzle_list[nozzle_id]);
        }
    }
    return result;
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders() const {
    return get_used_extruders(-1);
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders(int layer_id) const {
    std::set<int> used_extruders;
    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);
    for (auto filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) {
                used_extruders.insert(nozzle_opt->extruder_id);
            }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto &nozzle : nozzles) {
                used_extruders.insert(nozzle.extruder_id);
            }
        }
    }
    return std::vector<int>(used_extruders.begin(), used_extruders.end());
}

std::vector<int> LayeredNozzleGroupResult::get_extruder_map(bool zero_based, int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> extruder_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            extruder_map[idx] = _nozzle_list[nozzle_id].extruder_id;
        } else {
            extruder_map[idx] = -1;
        }
    }
    if (zero_based) {
        return extruder_map;
    }
    auto new_filament_map = extruder_map;
    std::transform(new_filament_map.begin(), new_filament_map.end(), new_filament_map.begin(), [](int val) { return val + 1; });
    return new_filament_map;
}

std::vector<int> LayeredNozzleGroupResult::get_nozzle_map(int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> nozzle_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            nozzle_map[idx] = _nozzle_list[nozzle_id].group_id;
        } else {
            nozzle_map[idx] = -1;
        }
    }
    return nozzle_map;
}

std::vector<int> LayeredNozzleGroupResult::get_volume_map(int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> volume_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            volume_map[idx] = _nozzle_list[nozzle_id].volume_type;
        } else {
            volume_map[idx] = -1;
        }
    }
    return volume_map;
}

std::vector<unsigned int> LayeredNozzleGroupResult::get_used_filaments(int layer_id) const {
    if (layer_id < 0 || layer_id >= static_cast<int>(_layer_filament_nozzle_maps.size())) {
        return _used_filaments;
    }
    if (!_layer_filament_sequences.empty() && layer_id < static_cast<int>(_layer_filament_sequences.size())) {
        return _layer_filament_sequences[layer_id];
    }
    return {};
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_for_filament(int filament_id, int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    if (filament_id < 0 || filament_id >= static_cast<int>(filament_nozzle_map.size())) {
        return std::nullopt;
    }
    int nozzle_id = filament_nozzle_map[filament_id];
    return get_nozzle_from_id(nozzle_id);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_nozzles_for_filament(int filament_id) const {
    std::set<int> nozzle_ids;
    if (!support_dynamic_nozzle_map) {
        if (filament_id >= 0 && filament_id < static_cast<int>(_default_filament_nozzle_map.size())) {
            nozzle_ids.insert(_default_filament_nozzle_map[filament_id]);
        }
    } else {
        for (const auto& map : _layer_filament_nozzle_maps) {
            if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
                nozzle_ids.insert(map[filament_id]);
            }
        }
    }
    std::vector<NozzleInfo> result;
    for (int id : nozzle_ids) {
        if (id >= 0 && id < static_cast<int>(_nozzle_list.size())) {
            result.push_back(_nozzle_list[id]);
        }
    }
    return result;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_first_nozzle_for_filament(int filament_id) const {
    if (filament_id < 0) return std::nullopt;
    if (!support_dynamic_nozzle_map) {
        if (filament_id >= static_cast<int>(_default_filament_nozzle_map.size())) return std::nullopt;
        return get_nozzle_from_id(_default_filament_nozzle_map[filament_id]);
    }
    for (size_t layer = 0; layer < _layer_filament_nozzle_maps.size(); ++layer) {
        auto layer_used_filaments = get_used_filaments(layer);
        if (std::find(layer_used_filaments.begin(), layer_used_filaments.end(), static_cast<unsigned int>(filament_id)) == layer_used_filaments.end()){
            continue;
        }
        const auto& map = _layer_filament_nozzle_maps[layer];
        if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
            int nozzle_id = map[filament_id];
            auto nozzle = get_nozzle_from_id(nozzle_id);
            if (nozzle) return nozzle;
        }
    }
    return std::nullopt;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_from_id(int nozzle_id) const {
    if (nozzle_id < 0 || nozzle_id >= static_cast<int>(_nozzle_list.size())) {
        return std::nullopt;
    }
    return _nozzle_list[nozzle_id];
}

int LayeredNozzleGroupResult::get_extruder_id(int filament_id, int layer_id) const {
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->extruder_id : -1;
}

int LayeredNozzleGroupResult::get_nozzle_id(int filament_id, int layer_id) const {
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->group_id : -1;
}

const std::vector<int>& LayeredNozzleGroupResult::get_layer_filament_nozzle_map(int layer_id) const {
    if (layer_id >= 0 && layer_id < static_cast<int>(_layer_filament_nozzle_maps.size())) {
        return _layer_filament_nozzle_maps[layer_id];
    }
    return _default_filament_nozzle_map;
}

// ==================== NozzleStatusRecorder Implementation ====================

bool NozzleStatusRecorder::is_nozzle_empty(int nozzle_id) const {
    auto iter = nozzle_filament_status.find(nozzle_id);
    return iter == nozzle_filament_status.end() || iter->second == -1;
}

int NozzleStatusRecorder::get_filament_in_nozzle(int nozzle_id) const {
    auto iter = nozzle_filament_status.find(nozzle_id);
    return iter != nozzle_filament_status.end() ? iter->second : -1;
}

int NozzleStatusRecorder::get_nozzle_in_extruder(int extruder_id) const {
    auto iter = extruder_nozzle_status.find(extruder_id);
    return iter != extruder_nozzle_status.end() ? iter->second : -1;
}

void NozzleStatusRecorder::clear_nozzle_status(int nozzle_id) {
    nozzle_filament_status[nozzle_id] = -1;
    for (auto& elem : extruder_nozzle_status) {
        if (elem.second == nozzle_id) {
            elem.second = -1;
        }
    }
}

void NozzleStatusRecorder::set_nozzle_status(int nozzle_id, int filament_id, int extruder_id) {
    nozzle_filament_status[nozzle_id] = filament_id;
    if (extruder_id != -1) {
        extruder_nozzle_status[extruder_id] = nozzle_id;
    }
}

// ==================== Helper Functions ====================

std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> nozzle_groups) {
    std::vector<NozzleInfo> ret;
    std::sort(nozzle_groups.begin(), nozzle_groups.end());
    int nozzle_id = 0;
    for (auto& group : nozzle_groups) {
        for (int i = 0; i < group.nozzle_count; ++i) {
            NozzleInfo tmp;
            tmp.diameter = group.diameter;
            tmp.extruder_id = group.extruder_id;
            tmp.volume_type = group.volume_type;
            tmp.group_id = nozzle_id++;
            ret.emplace_back(std::move(tmp));
        }
    }
    return ret;
}

std::vector<NozzleInfo> build_nozzle_list(double diameter, const std::vector<int>& filament_nozzle_map,
                                          const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map) {
    std::string diameter_str = format_diameter_to_str(diameter);
    std::map<int, std::vector<int>> nozzle_to_filaments;
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        nozzle_to_filaments[nozzle_id].emplace_back(static_cast<int>(idx));
    }
    std::vector<NozzleInfo> ret;
    for (auto& elem : nozzle_to_filaments) {
        int nozzle_id = elem.first;
        auto& filaments = elem.second;
        if (filaments.empty()) continue;
        NozzleInfo info;
        info.diameter = diameter_str;
        info.group_id = nozzle_id;
        info.extruder_id = filament_map[filaments.front()];
        info.volume_type = NozzleVolumeType(filament_volume_map[filaments.front()]);
        ret.emplace_back(std::move(info));
    }
    return ret;
}

// Parser for extruder_nozzle_stats (e.g. "0.4:4,0.6:4" per extruder stats)
std::vector<std::map<NozzleVolumeType, int>> get_extruder_nozzle_stats(const std::vector<std::string>& stats_strings) {
    VORTEK_LOG(debug, "get_extruder_nozzle_stats: stats size = " << stats_strings.size());
    std::vector<std::map<NozzleVolumeType, int>> ret;
    for (const auto& stat_str : stats_strings) {
        std::map<NozzleVolumeType, int> extruder_stats;
        // Parse a string like "0.4:Standard:4,0.6:High Flow:4" or similar
        // Let's implement a robust parser based on ':' and ','
        std::stringstream ss(stat_str);
        std::string group_token;
        while (std::getline(ss, group_token, ',')) {
            if (group_token.empty()) continue;
            std::stringstream gss(group_token);
            std::vector<std::string> parts;
            std::string part;
            while (std::getline(gss, part, ':')) {
                parts.push_back(part);
            }
            if (parts.size() >= 3) {
                // Format: diameter:volume_type:count
                try {
                    int vol_type_val = std::stoi(parts[1]);
                    int count = std::stoi(parts[2]);
                    extruder_stats[static_cast<NozzleVolumeType>(vol_type_val)] = count;
                } catch (...) {
                    // Fallback to text parsing
                    std::string vol_str = parts[1];
                    NozzleVolumeType type = NozzleVolumeType::nvtStandard;
                    if (vol_str == "High Flow" || vol_str == "1") {
                        type = NozzleVolumeType::nvtHighFlow;
                    }
                    try {
                        int count = std::stoi(parts[2]);
                        extruder_stats[type] = count;
                    } catch (...) {}
                }
            } else if (parts.size() == 2) {
                // Format: volume_type:count (fallback)
                try {
                    int vol_type_val = std::stoi(parts[0]);
                    int count = std::stoi(parts[1]);
                    extruder_stats[static_cast<NozzleVolumeType>(vol_type_val)] = count;
                } catch (...) {
                    std::string vol_str = parts[0];
                    NozzleVolumeType type = NozzleVolumeType::nvtStandard;
                    if (vol_str == "High Flow" || vol_str == "1") {
                        type = NozzleVolumeType::nvtHighFlow;
                    }
                    try {
                        int count = std::stoi(parts[1]);
                        extruder_stats[type] = count;
                    } catch (...) {}
                }
            }
        }
        ret.push_back(extruder_stats);
    }
    return ret;
}

} // namespace MultiNozzleUtils
} // namespace Slic3r
