#pragma once

#include <vector>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;

enum class AlignOp : int {
    DistributeX, DistributeY, DistributeZ,
    XMin, XCenter, XMax,
    YMin, YCenter, YMax,
    ZMin, ZCenter, ZMax,
};

struct AlignOpInfo
{
    AlignOp     op;
    int         axis;
    const char* label;
    const char* axis_suffix;
    const char* icon;
};

// In menu order. The label is marked with L() and translated where it is shown.
const std::vector<AlignOpInfo>& align_ops();
const AlignOpInfo&              align_op_info(AlignOp op);

struct AxisExtent
{
    double min;
    double max;
};

// The offset along the op's axis for each item. The reference is the union of all items:
// align moves each item's min, centre or max to the union's, distribute keeps the items with the
// lowest and highest centre and spaces the centres of the others evenly between them.
std::vector<double> align_distribute_offsets(const std::vector<AxisExtent>& items, AlignOp op);

bool can_align_distribute(GLCanvas3D& canvas, AlignOp op);
bool align_distribute(GLCanvas3D& canvas, AlignOp op);

} // namespace GUI
} // namespace Slic3r
