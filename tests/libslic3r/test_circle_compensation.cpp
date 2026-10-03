#include <catch2/catch_all.hpp>

#include "libslic3r/CircleCompensation.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Coefficients of "Bambu PLA Basic @BBL H2C" (resources/profiles/BBL/filament).
CircleCompensationParams pla_basic_h2c()
{
    CircleCompensationParams p;
    p.speed                 = 200.;
    p.counter_speed_coef    = 0.;
    p.counter_diameter_coef = 0.0025;
    p.counter_constant      = 0.014;
    p.hole_speed_coef       = 0.;
    p.hole_diameter_coef    = -0.0028;
    p.hole_constant         = 0.12;
    p.counter_limit_min     = 0.014;
    p.counter_limit_max     = 0.076;
    p.hole_limit_min        = 0.05;
    p.hole_limit_max        = 0.12;
    p.diameter_limit        = 50.;
    return p;
}

// BambuStudio's radial offset, written out independently of CircleCompensationParams.
double bs_offset(double c1, double c2, double c3, double lo, double hi, double speed, double d)
{
    double v = c1 * speed + c2 * d + c3;
    if (v < lo)
        v = lo;
    else if (v > hi)
        v = hi;
    return v;
}

Polygon circle(double diameter_mm, size_t n = 360)
{
    Polygon p;
    const double r = scale_(diameter_mm / 2.);
    for (size_t i = 0; i < n; ++i) {
        const double a = 2. * M_PI * double(i) / double(n);
        p.points.emplace_back(coord_t(std::round(r * std::cos(a))), coord_t(std::round(r * std::sin(a))));
    }
    return p;
}

Polygon square(double side_mm)
{
    const coord_t h = scale_(side_mm / 2.);
    return Polygon{{-h, -h}, {h, -h}, {h, h}, {-h, h}};
}

Polygon hole(Polygon p)
{
    p.make_clockwise();
    return p;
}

double measured_diameter(const Polygon &p)
{
    Point  center;
    double diameter = 0.;
    REQUIRE(p.is_approx_circle(scale_(0.5), 5 * scale_(0.01) * scale_(0.01), center, diameter));
    return unscale<double>(diameter);
}

} // namespace

TEST_CASE("A round hole grows by twice the radial offset of the BambuStudio formula", "[CircleCompensation]")
{
    const CircleCompensationParams p = pla_basic_h2c();
    // 10 mm: 0.12 - 0.0028 * 10 = 0.092, inside [0.05, 0.12]. 40 mm: 0.008, clamped up to 0.05.
    const double d = GENERATE(3., 10., 40.);

    Surface surface(stInternal, ExPolygon(square(80.), hole(circle(d))));
    const double before = measured_diameter(surface.expolygon.holes.front());
    apply_circle_compensation(surface, p, 0.);

    const double expected = before + 2. * bs_offset(p.hole_speed_coef, p.hole_diameter_coef, p.hole_constant, p.hole_limit_min,
                                                    p.hole_limit_max, p.speed, before);
    REQUIRE(surface.expolygon.holes.size() == 1);
    CHECK_THAT(measured_diameter(surface.expolygon.holes.front()), WithinAbs(expected, 0.002));
    CHECK(surface.expolygon.holes.front().is_clockwise());
    CHECK(surface.expolygon.contour.points == square(80.).points);
    REQUIRE(surface.holes_circle_compensation.size() == 1);
    CHECK(surface.holes_circle_compensation.front() == 0);
    CHECK_FALSE(surface.counter_circle_compensation);
}

TEST_CASE("A round contour grows by twice the radial offset of the BambuStudio formula", "[CircleCompensation]")
{
    const CircleCompensationParams p = pla_basic_h2c();
    // 8 mm: 0.014 + 0.0025 * 8 = 0.034. 30 mm: 0.089, clamped down to 0.076.
    const double d = GENERATE(8., 30.);

    Surface surface(stInternal, ExPolygon(circle(d)));
    const double before = measured_diameter(surface.expolygon.contour);
    apply_circle_compensation(surface, p, 0.);

    const double expected = before + 2. * bs_offset(p.counter_speed_coef, p.counter_diameter_coef, p.counter_constant,
                                                    p.counter_limit_min, p.counter_limit_max, p.speed, before);
    CHECK_THAT(measured_diameter(surface.expolygon.contour), WithinAbs(expected, 0.002));
    CHECK(surface.expolygon.contour.is_counter_clockwise());
    CHECK(surface.counter_circle_compensation);
}

TEST_CASE("A positive manual offset loosens the fit by that much in diameter", "[CircleCompensation]")
{
    const CircleCompensationParams p = pla_basic_h2c();
    const double manual = 0.1;

    Surface plain(stInternal, ExPolygon(circle(30.), hole(circle(10.))));
    Surface loose = plain;
    apply_circle_compensation(plain, p, 0.);
    apply_circle_compensation(loose, p, manual);

    CHECK_THAT(measured_diameter(loose.expolygon.holes.front()) - measured_diameter(plain.expolygon.holes.front()), WithinAbs(manual, 0.002));
    CHECK_THAT(measured_diameter(loose.expolygon.contour) - measured_diameter(plain.expolygon.contour), WithinAbs(-manual, 0.002));
}

TEST_CASE("Only circles below the diameter limit are flagged for the circle speed", "[CircleCompensation]")
{
    CircleCompensationParams p = pla_basic_h2c();
    p.diameter_limit = 20.;

    Surface surface(stInternal, ExPolygon(circle(60.), hole(circle(25.))));
    surface.expolygon.holes.push_back(hole(circle(5.)));
    for (Point &pt : surface.expolygon.holes.back().points)
        pt += Point(coord_t(scale_(20.)), coord_t(0));
    const double big_before = measured_diameter(surface.expolygon.holes.front());
    apply_circle_compensation(surface, p, 0.);

    CHECK_FALSE(surface.counter_circle_compensation);
    REQUIRE(surface.holes_circle_compensation.size() == 1);
    CHECK(surface.holes_circle_compensation.front() == 1);
    CHECK(measured_diameter(surface.expolygon.holes.front()) > big_before);
}

TEST_CASE("Non-circular outlines and polyhole holes are left alone", "[CircleCompensation]")
{
    const CircleCompensationParams p = pla_basic_h2c();

    Surface squares(stInternal, ExPolygon(square(40.), hole(square(10.))));
    const ExPolygon squares_before = squares.expolygon;
    apply_circle_compensation(squares, p, 0.);
    CHECK(squares.expolygon.contour.points == squares_before.contour.points);
    CHECK(squares.expolygon.holes.front().points == squares_before.holes.front().points);

    Surface polyhole(stInternal, ExPolygon(circle(30.), hole(circle(10.))));
    const Polygon hole_before = polyhole.expolygon.holes.front();
    apply_circle_compensation(polyhole, p, 0., true);
    CHECK(polyhole.expolygon.holes.front().points == hole_before.points);
    CHECK(polyhole.holes_circle_compensation.empty());
    CHECK(polyhole.counter_circle_compensation);
}

TEST_CASE("Coefficients are read for the wall filament", "[CircleCompensation]")
{
    PrintConfig config;
    config.circle_compensation_speed.values = {200., 150.};
    config.counter_coef_1.values            = {0., 0.001};
    config.counter_coef_2.values            = {0.0025, 0.0058};
    config.counter_coef_3.values            = {0.014, 0.0107};
    config.hole_coef_1.values               = {0., 0.002};
    config.hole_coef_2.values               = {-0.0028, -0.0042};
    config.hole_coef_3.values               = {0.12, 0.2006};
    config.counter_limit_min.values         = {0.014, 0.01};
    config.counter_limit_max.values         = {0.076, 0.15};
    config.hole_limit_min.values            = {0.05, 0.09};
    config.hole_limit_max.values            = {0.12, 0.2};
    config.diameter_limit.values            = {50., 30.};

    const CircleCompensationParams p = CircleCompensationParams::from_config(config, 1);
    CHECK(p.speed == 150.);
    CHECK(p.diameter_limit == 30.);
    // 0.001 * 150 + 0.0058 * 10 + 0.0107 = 0.2187, clamped to 0.15.
    CHECK_THAT(p.counter_offset(10.), WithinAbs(0.15, 1e-9));
    // 0.002 * 150 - 0.0042 * 10 + 0.2006 = 0.4586, clamped to 0.2.
    CHECK_THAT(p.hole_offset(10.), WithinAbs(0.2, 1e-9));
    CHECK_THAT(CircleCompensationParams::from_config(config, 0).hole_offset(10.), WithinAbs(0.092, 1e-9));
}
