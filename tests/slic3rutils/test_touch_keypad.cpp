#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/TouchKeypad.hpp"

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("A field value parses as a number with its suffix kept apart", "[TouchKeypad]")
{
    const auto [text, number, value, decimals, suffix] = GENERATE(table<std::string, std::string, double, int, std::string>({
        {"200", "200", 200., 0, ""},
        {"0.2", "0.2", 0.2, 1, ""},
        {" -1.25 ", "-1.25", -1.25, 2, ""},
        {"+3", "+3", 3., 0, ""},
        {".5", ".5", 0.5, 1, ""},
        {"15%", "15", 15., 0, "%"},
        {"0.4 mm", "0.4", 0.4, 1, " mm"},
        {"12mm", "12", 12., 0, "mm"},
    }));
    const auto parsed = parse_touch_number(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->number == number);
    CHECK_THAT(parsed->value, WithinAbs(value, 1e-12));
    CHECK(parsed->decimals == decimals);
    CHECK(parsed->suffix == suffix);
}

TEST_CASE("Lists, words and bare signs are not numbers", "[TouchKeypad]")
{
    const std::string text = GENERATE("", "  ", "0.2,0.2", "abc", "N/A", "-", ".", "%", "mm", "1e5", "1.2.3", "5 %%");
    CHECK_FALSE(parse_touch_number(text).has_value());
}

TEST_CASE("Step sizes follow the precision of the original value", "[TouchKeypad]")
{
    const auto [text, fine, coarse, decimals] = GENERATE(table<std::string, double, double, int>({
        {"200", 1., 10., 0},
        {"-3", 1., 10., 0},
        {"0.2", 0.01, 0.1, 2},
        {"0.125", 0.001, 0.01, 3},
        {"1.5", 0.1, 1., 1},
        {"12.75", 0.01, 0.1, 2},
        {"15%", 1., 10., 0},
    }));
    const TouchSteps steps = touch_steps(*parse_touch_number(text));
    CHECK_THAT(steps.fine, WithinAbs(fine, 1e-12));
    CHECK_THAT(steps.coarse, WithinAbs(coarse, 1e-12));
    CHECK(steps.decimals == decimals);
}

TEST_CASE("Formatting drops trailing zeros down to the minimum and never shows -0", "[TouchKeypad]")
{
    CHECK(format_touch_number(0.30000000004, 1, 2) == "0.3");
    CHECK(format_touch_number(0.21, 1, 2) == "0.21");
    CHECK(format_touch_number(2., 0, 2) == "2");
    CHECK(format_touch_number(2., 2, 2) == "2.00");
    CHECK(format_touch_number(210., 0, 0) == "210");
    CHECK(format_touch_number(-0.001, 0, 2) == "0");
    CHECK(format_touch_number(-0.5, 0, 2) == "-0.5");
}

TEST_CASE("Stepping keeps the field's precision and adds no float noise", "[TouchKeypad]")
{
    const auto [text, delta, expected] = GENERATE(table<std::string, double, std::string>({
        {"0.2", 0.01, "0.21"},
        {"0.2", 0.1, "0.3"},
        {"0.2", -0.3, "-0.1"},
        {"200", 10., "210"},
        {"200", -1., "199"},
        {"1.5", 0.1, "1.6"},
        {"0.1", -0.1, "0.0"},
    }));
    const auto       parsed = parse_touch_number(text);
    const TouchSteps steps  = touch_steps(*parsed);
    CHECK(step_touch_number(parsed->number, delta, parsed->decimals, steps) == expected);
}

TEST_CASE("Stepping an empty or partial entry starts from zero", "[TouchKeypad]")
{
    const TouchSteps steps{1., 10., 0};
    CHECK(step_touch_number("", 1., 0, steps) == "1");
    CHECK(step_touch_number("-", -10., 0, steps) == "-10");
}

TEST_CASE("Stepping keeps digits typed beyond the step precision", "[TouchKeypad]")
{
    const TouchSteps steps = touch_steps(*parse_touch_number("0.2"));
    CHECK(step_touch_number("0.215", 0.01, 1, steps) == "0.225");
}
