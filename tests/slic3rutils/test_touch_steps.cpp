#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cfloat>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/TouchSteps.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

static double step_for(const std::string& opt_key, double value, bool percent = false)
{
    const ConfigOptionDef* def = print_config_def.get(opt_key.substr(0, opt_key.find('#')));
    REQUIRE(def != nullptr);
    return touch_step_for_option(*def, opt_key, value, percent);
}

TEST_CASE("Touch steps suit the options people change", "[TouchSteps]")
{
    const auto [key, value, percent, step] = GENERATE(table<std::string, double, bool, double>({
        {"layer_height", 0.2, false, 0.02},
        {"initial_layer_print_height", 0.2, false, 0.02},
        {"nozzle_temperature", 220., false, 5.},
        {"nozzle_temperature_initial_layer", 220., false, 5.},
        {"hot_plate_temp", 60., false, 5.},
        {"chamber_temperature", 0., false, 5.},
        {"outer_wall_speed", 60., false, 10.},
        {"travel_speed", 300., false, 10.},
        {"inner_wall_speed#1", 150., false, 10.},
        {"slow_down_min_speed", 10., false, 1.},
        {"internal_bridge_speed", 150., true, 5.},
        {"default_acceleration", 5000., false, 500.},
        {"initial_layer_acceleration", 300., false, 100.},
        {"outer_wall_jerk", 9., false, 1.},
        {"sparse_infill_density", 15., true, 5.},
        {"fan_max_speed", 100., false, 5.},
        {"overhang_fan_speed", 100., false, 5.},
        {"line_width", 0.42, false, 0.02},
        {"line_width", 105., true, 5.},
        {"outer_wall_line_width", 0.42, false, 0.02},
        {"retraction_length", 0.8, false, 0.1},
        {"retraction_speed", 30., false, 5.},
        {"wall_loops", 2., false, 1.},
        {"top_shell_layers", 4., false, 1.},
        {"skirt_loops", 1., false, 1.},
        {"z_offset", 0., false, 0.01},
        {"filament_flow_ratio", 0.98, false, 0.01},
        {"bridge_flow", 1., false, 0.01},
        {"pressure_advance", 0.02, false, 0.001},
        {"filament_diameter", 1.75, false, 0.01},
        {"filament_max_volumetric_speed", 21., false, 1.},
        {"brim_width", 5., false, 1.},
        {"brim_width", 0., false, 0.1},
        {"infill_direction", 45., false, 5.},
        {"support_threshold_angle", 30., false, 5.},
        {"tree_support_branch_diameter_angle", 5., false, 1.},
        {"slow_down_layer_time", 5., false, 1.},
        {"fan_cooling_layer_time", 60., false, 5.},
        {"ironing_spacing", 0.1, false, 0.01},
    }));
    INFO(key << " = " << value << (percent ? "%" : ""));
    CHECK_THAT(step_for(key, value, percent), WithinAbs(step, 1e-9));
}

TEST_CASE("Integer options always step by a whole number", "[TouchSteps]")
{
    const std::string key = GENERATE("wall_loops", "top_shell_layers", "tree_support_wall_count", "skirt_loops", "nozzle_temperature");
    const double      step = step_for(key, 1.);
    CHECK(step >= 1.);
    CHECK(step == std::round(step));
}

TEST_CASE("A step lands on the step grid and stays inside the option's range", "[TouchSteps]")
{
    const auto [value, step, direction, min, max, expected] = GENERATE(table<double, double, int, double, double, double>({
        {200., 5., 1, 0., 1500., 205.},
        {203., 5., 1, 0., 1500., 205.},
        {203., 5., -1, 0., 1500., 200.},
        {0.2, 0.02, 1, 0., FLT_MAX, 0.22},
        {0.2, 0.02, -1, 0., FLT_MAX, 0.18},
        {0.23, 0.02, 1, 0., FLT_MAX, 0.24},
        {98., 5., 1, 0., 100., 100.},
        {0., 0.1, -1, 0., FLT_MAX, 0.},
        {0., 0.01, -1, -FLT_MAX, FLT_MAX, -0.01},
    }));
    CHECK_THAT(touch_step_value(value, step, direction, min, max), WithinAbs(expected, 1e-9));
}

TEST_CASE("Step decimals cover the step exactly", "[TouchSteps]")
{
    CHECK(touch_step_decimals(5.) == 0);
    CHECK(touch_step_decimals(0.1) == 1);
    CHECK(touch_step_decimals(0.02) == 2);
    CHECK(touch_step_decimals(0.005) == 3);
}
