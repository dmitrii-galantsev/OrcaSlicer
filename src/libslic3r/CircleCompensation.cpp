#include "CircleCompensation.hpp"

#include "ClipperUtils.hpp"
#include "PrintConfig.hpp"
#include "Surface.hpp"

#include <algorithm>

namespace Slic3r {

// Circle detection tolerances of BambuStudio's LayerRegion::auto_circle_compensation().
static const double circle_max_deviation = scale_(0.5);
static const double circle_max_variance  = 5 * scale_(0.01) * scale_(0.01);

CircleCompensationParams CircleCompensationParams::from_config(const PrintConfig &config, size_t filament_idx)
{
    CircleCompensationParams p;
    p.speed                 = config.circle_compensation_speed.get_at(filament_idx);
    p.counter_speed_coef    = config.counter_coef_1.get_at(filament_idx);
    p.counter_diameter_coef = config.counter_coef_2.get_at(filament_idx);
    p.counter_constant      = config.counter_coef_3.get_at(filament_idx);
    p.hole_speed_coef       = config.hole_coef_1.get_at(filament_idx);
    p.hole_diameter_coef    = config.hole_coef_2.get_at(filament_idx);
    p.hole_constant         = config.hole_coef_3.get_at(filament_idx);
    p.counter_limit_min     = config.counter_limit_min.get_at(filament_idx);
    p.counter_limit_max     = config.counter_limit_max.get_at(filament_idx);
    p.hole_limit_min        = config.hole_limit_min.get_at(filament_idx);
    p.hole_limit_max        = config.hole_limit_max.get_at(filament_idx);
    p.diameter_limit        = config.diameter_limit.get_at(filament_idx);
    return p;
}

// Not std::clamp: BambuStudio tests the lower limit first, which decides the result when min > max.
static double clamp_offset(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

double CircleCompensationParams::counter_offset(double diameter) const
{
    return clamp_offset(counter_speed_coef * speed + counter_diameter_coef * diameter + counter_constant, counter_limit_min, counter_limit_max);
}

double CircleCompensationParams::hole_offset(double diameter) const
{
    return clamp_offset(hole_speed_coef * speed + hole_diameter_coef * diameter + hole_constant, hole_limit_min, hole_limit_max);
}

void apply_circle_compensation(Surface &surface, const CircleCompensationParams &params, double manual_offset, bool skip_holes)
{
    const double half_manual = scale_(manual_offset) / 2.;
    const double diameter_limit = scale_(params.diameter_limit);
    Point  center;
    double diameter = 0.;

    surface.counter_circle_compensation = false;
    surface.holes_circle_compensation.clear();

    if (surface.expolygon.contour.is_approx_circle(circle_max_deviation, circle_max_variance, center, diameter)) {
        const double delta = scale_(params.counter_offset(unscale<double>(diameter))) - half_manual;
        Polygons     out   = offset(surface.expolygon.contour, float(delta));
        if (out.size() == 1) {
            surface.expolygon.contour = std::move(out.front());
            surface.counter_circle_compensation = diameter < diameter_limit;
        }
    }
    if (skip_holes)
        return;
    for (size_t i = 0; i < surface.expolygon.holes.size(); ++i) {
        Polygon &hole = surface.expolygon.holes[i];
        if (!hole.is_approx_circle(circle_max_deviation, circle_max_variance, center, diameter))
            continue;
        // A positive offset of a CW hole shrinks it, so the sign is flipped to make a positive compensation grow the hole.
        const double delta = -scale_(params.hole_offset(unscale<double>(diameter))) - half_manual;
        Polygons     out   = offset(hole, float(delta));
        if (out.size() == 1) {
            hole = std::move(out.front());
            if (diameter < diameter_limit)
                surface.holes_circle_compensation.push_back(int(i));
        }
    }
}

} // namespace Slic3r
