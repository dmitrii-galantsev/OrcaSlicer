#ifndef slic3r_CircleCompensation_hpp_
#define slic3r_CircleCompensation_hpp_

#include "libslic3r.h"

namespace Slic3r {

class PrintConfig;
class Surface;

// BambuStudio's "auto circle contour-hole compensation". All lengths in mm, the speed in mm/s.
struct CircleCompensationParams
{
    double speed                 = 200.;
    double counter_speed_coef    = 0.;
    double counter_diameter_coef = 0.025;
    double counter_constant      = -0.11;
    double hole_speed_coef       = 0.;
    double hole_diameter_coef    = -0.025;
    double hole_constant         = 0.28;
    double counter_limit_min     = -0.04;
    double counter_limit_max     = 0.05;
    double hole_limit_min        = 0.08;
    double hole_limit_max        = 0.25;
    double diameter_limit        = 50.;

    static CircleCompensationParams from_config(const PrintConfig &config, size_t filament_idx);

    // Radial offsets for a circle of the given diameter; a positive value makes the circle bigger.
    double counter_offset(double diameter) const;
    double hole_offset(double diameter) const;
};

// Detects circular contours and holes of surface.expolygon and offsets each by the compensation for its diameter.
// manual_offset is the user's fit adjustment in mm of diameter: positive grows holes and shrinks contours.
// Records the circles smaller than diameter_limit in counter_circle_compensation / holes_circle_compensation.
// skip_holes leaves the holes alone (they are converted to polyholes instead).
void apply_circle_compensation(Surface &surface, const CircleCompensationParams &params, double manual_offset, bool skip_holes = false);

} // namespace Slic3r

#endif
