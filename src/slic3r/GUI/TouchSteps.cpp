#include "TouchSteps.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>

#include "libslic3r/Config.hpp"

namespace Slic3r { namespace GUI {

namespace {

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// Options whose useful step is finer or coarser than their unit suggests.
const std::map<std::string, double>& step_by_key()
{
    static const std::map<std::string, double> steps{
        {"layer_height", 0.02},
        {"initial_layer_print_height", 0.02},
        {"min_layer_height", 0.02},
        {"max_layer_height", 0.02},
        {"z_offset", 0.01},
        {"zaa_min_z", 0.01},
        {"z_hop", 0.05},
        {"retraction_length", 0.1},
        {"retract_restart_extra", 0.1},
        {"retraction_distances_when_ec", 0.1},
        {"filament_retract_length_nc", 0.1},
        {"retraction_speed", 5.},
        {"deretraction_speed", 5.},
        {"deretract_speed_extruder_change", 5.},
        {"pressure_advance", 0.001},
        {"adaptive_pressure_advance_bridges", 0.001},
        {"filament_diameter", 0.01},
        {"nozzle_diameter", 0.05},
        {"filament_density", 0.01},
        {"filament_shrink", 0.1},
        {"filament_shrinkage_compensation_z", 0.1},
        {"elefant_foot_compensation", 0.05},
        {"xy_hole_compensation", 0.01},
        {"xy_contour_compensation", 0.01},
        {"support_bottom_z_distance", 0.02},
        {"support_object_xy_distance", 0.05},
        {"support_object_first_layer_gap", 0.05},
        {"raft_contact_distance", 0.05},
        {"brim_object_gap", 0.05},
        {"ironing_spacing", 0.01},
        {"support_ironing_spacing", 0.01},
        {"filament_ironing_spacing", 0.01},
        {"resolution", 0.005},
        {"wall_maximum_resolution", 0.005},
        {"wall_maximum_deviation", 0.005},
        {"slice_closing_radius", 0.01},
        {"default_junction_deviation", 0.005},
        {"machine_max_junction_deviation", 0.005},
        {"fuzzy_skin_thickness", 0.05},
        {"fuzzy_skin_point_distance", 0.05},
    };
    return steps;
}

// "mm/s or %" -> "mm/s"
std::string unit_of(const ConfigOptionDef& def)
{
    const std::string& side = def.sidetext;
    const size_t       or_pos = side.find(" or ");
    return or_pos == std::string::npos ? side : side.substr(0, or_pos);
}

bool is_integer_type(ConfigOptionType type) { return type == coInt || type == coInts; }

double unit_step(const ConfigOptionDef& def, const std::string& key, const std::string& unit, double value)
{
    const double magnitude = std::abs(value);
    const bool   bounded   = def.min > -FLT_MAX && def.max < FLT_MAX;

    if (ends_with(key, "line_width"))
        return 0.02;
    if (contains(unit, "\u2103") || contains(key, "temperature") || ends_with(key, "_temp"))
        return 5.;
    if (starts_with(unit, "mm/s\u00B2"))
        return magnitude >= 1000. ? 500. : 100.;
    if (contains(key, "jerk"))
        return 1.;
    if (unit == "mm/s")
        return magnitude >= 20. ? 10. : 1.;
    if (starts_with(unit, "mm\u00B3/s"))
        return 1.;
    if (unit == "mm\u00B3")
        return magnitude >= 20. ? 5. : 1.;
    if (unit == "\u00B0")
        return bounded && def.max - def.min <= 30. ? 1. : 5.;
    if (unit == "layers" || unit == "layer")
        return 1.;
    if (unit == "%")
        return 5.;
    if (unit == "s")
        return magnitude >= 20. ? 5. : magnitude > 0. && magnitude < 1. ? 0.1 : 1.;
    if (unit.empty()) {
        if (is_integer_type(def.type))
            return 1.;
        // Flow ratios, damping factors and other unitless ratios around 1.
        if (def.max <= 2.f)
            return 0.01;
        return magnitude >= 10. ? 1. : 0.1;
    }
    // mm, mm², g, N, Hz, money and anything else measured in plain units.
    if (def.max <= 1.f)
        return 0.01;
    return magnitude >= 1. ? 1. : 0.1;
}

double nice_step_below(double limit)
{
    const double base = std::pow(10., std::floor(std::log10(limit)));
    for (double m : {5., 2., 1.})
        if (m * base <= limit)
            return m * base;
    return base;
}

} // namespace

double touch_step_for_option(const ConfigOptionDef& def, const std::string& opt_key, double value, bool percent)
{
    const std::string key = opt_key.substr(0, opt_key.find('#'));

    double step;
    if (const auto it = step_by_key().find(key); it != step_by_key().end())
        step = it->second;
    else if (percent && def.type != coPercent && def.type != coPercents)
        step = 5.;
    else
        step = unit_step(def, key, percent ? std::string("%") : unit_of(def), value);

    // Ranges like 0..2 layers or 0..15° would be crossed in two or three presses.
    if (def.min > -FLT_MAX && def.max < FLT_MAX && def.max > def.min && (def.max - def.min) / step < 5.)
        step = nice_step_below((def.max - def.min) / 10.);

    if (is_integer_type(def.type))
        step = std::max(1., std::round(step));
    return step;
}

double touch_step_value(double value, double step, int direction, double min, double max)
{
    // The tolerance keeps 0.2 / 0.02 == 9.999... on the grid point 10.
    const double eps   = 1e-6;
    const double steps = value / step;
    const double index = direction > 0 ? std::floor(steps + eps) + 1. : std::ceil(steps - eps) - 1.;
    double       next  = index * step;
    if (min > -FLT_MAX)
        next = std::max(next, min);
    if (max < FLT_MAX)
        next = std::min(next, max);
    return next;
}

int touch_step_decimals(double step)
{
    for (int decimals = 0; decimals < 6; ++decimals) {
        const double scaled = step * std::pow(10., decimals);
        if (std::abs(scaled - std::round(scaled)) < 1e-6)
            return decimals;
    }
    return 6;
}

}} // namespace Slic3r::GUI
