#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/AlignDistribute.hpp"

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

static std::vector<AxisExtent> moved(const std::vector<AxisExtent>& items, AlignOp op)
{
    const std::vector<double> off = align_distribute_offsets(items, op);
    REQUIRE(off.size() == items.size());
    std::vector<AxisExtent> out;
    for (size_t i = 0; i < items.size(); ++i)
        out.push_back({items[i].min + off[i], items[i].max + off[i]});
    return out;
}

// Three items of widths 10, 4 and 20, spanning 0..60 together.
static const std::vector<AxisExtent> three = {{40., 50.}, {0., 4.}, {20., 40.}};

TEST_CASE("Align min moves every item's low edge to the lowest edge of the selection", "[AlignDistribute]")
{
    const AlignOp op = GENERATE(AlignOp::XMin, AlignOp::YMin, AlignOp::ZMin);
    for (const AxisExtent& e : moved(three, op)) {
        CHECK_THAT(e.min, WithinAbs(0., 1e-9));
    }
}

TEST_CASE("Align max moves every item's high edge to the highest edge of the selection", "[AlignDistribute]")
{
    const AlignOp op = GENERATE(AlignOp::XMax, AlignOp::YMax, AlignOp::ZMax);
    for (const AxisExtent& e : moved(three, op)) {
        CHECK_THAT(e.max, WithinAbs(50., 1e-9));
    }
}

TEST_CASE("Align center centres every item on the centre of the selection's bounding box", "[AlignDistribute]")
{
    const AlignOp op = GENERATE(AlignOp::XCenter, AlignOp::YCenter, AlignOp::ZCenter);
    const std::vector<AxisExtent> after = moved(three, op);
    for (const AxisExtent& e : after) {
        CHECK_THAT(0.5 * (e.min + e.max), WithinAbs(25., 1e-9));
    }
    CHECK_THAT(after[2].max - after[2].min, WithinAbs(20., 1e-9));
}

TEST_CASE("Distribute keeps the outer items and spaces all centres evenly", "[AlignDistribute]")
{
    const AlignOp op = GENERATE(AlignOp::DistributeX, AlignOp::DistributeY, AlignOp::DistributeZ);
    const std::vector<AxisExtent> items = {{90., 110.}, {-5., 5.}, {10., 14.}, {30., 50.}};
    const std::vector<double>     off   = align_distribute_offsets(items, op);
    CHECK_THAT(off[0], WithinAbs(0., 1e-9));
    CHECK_THAT(off[1], WithinAbs(0., 1e-9));
    const std::vector<AxisExtent> after = moved(items, op);
    auto center = [&](size_t i) { return 0.5 * (after[i].min + after[i].max); };
    CHECK_THAT(center(2), WithinAbs(100. / 3., 1e-9));
    CHECK_THAT(center(3), WithinAbs(200. / 3., 1e-9));
}

TEST_CASE("Distribute leaves fewer than three items where they are", "[AlignDistribute]")
{
    const std::vector<AxisExtent> two = {{0., 10.}, {50., 60.}};
    for (double o : align_distribute_offsets(two, AlignOp::DistributeX)) {
        CHECK(o == 0.);
    }
    CHECK(align_distribute_offsets({}, AlignOp::XMin).empty());
}

TEST_CASE("Every op is listed once, in enum order, with the axis its name says", "[AlignDistribute]")
{
    const std::vector<AlignOpInfo>& ops = align_ops();
    REQUIRE(ops.size() == 12);
    for (size_t i = 0; i < ops.size(); ++i) {
        CHECK(static_cast<size_t>(ops[i].op) == i);
        CHECK(&align_op_info(ops[i].op) == &ops[i]);
    }
    CHECK(align_op_info(AlignOp::XCenter).axis == 0);
    CHECK(align_op_info(AlignOp::YMax).axis == 1);
    CHECK(align_op_info(AlignOp::DistributeZ).axis == 2);
}
