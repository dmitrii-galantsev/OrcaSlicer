#include "AlignDistribute.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
namespace GUI {

const std::vector<AlignOpInfo>& align_ops()
{
    static const std::vector<AlignOpInfo> ops = {
        {AlignOp::DistributeX, 0, L("Distribute left-right"), " (X)", "menu_distribute_x"},
        {AlignOp::DistributeY, 1, L("Distribute front-back"), " (Y)", "menu_distribute_y"},
        {AlignOp::DistributeZ, 2, L("Distribute top-bottom"), " (Z)", "menu_distribute_z"},
        {AlignOp::XMin, 0, L("Align left"), " (-X)", "menu_align_x_min"},
        {AlignOp::XCenter, 0, L("Align left-right center"), " (X)", "menu_align_x_center"},
        {AlignOp::XMax, 0, L("Align right"), " (+X)", "menu_align_x_max"},
        {AlignOp::YMin, 1, L("Align front"), " (-Y)", "menu_align_y_min"},
        {AlignOp::YCenter, 1, L("Align front-back center"), " (Y)", "menu_align_y_center"},
        {AlignOp::YMax, 1, L("Align back"), " (+Y)", "menu_align_y_max"},
        {AlignOp::ZMin, 2, L("Align bottom"), " (-Z)", "menu_align_z_min"},
        {AlignOp::ZCenter, 2, L("Align top-bottom center"), " (Z)", "menu_align_z_center"},
        {AlignOp::ZMax, 2, L("Align top"), " (+Z)", "menu_align_z_max"},
    };
    return ops;
}

const AlignOpInfo& align_op_info(AlignOp op)
{
    return align_ops()[static_cast<size_t>(op)];
}

static bool is_distribute(AlignOp op)
{
    return op == AlignOp::DistributeX || op == AlignOp::DistributeY || op == AlignOp::DistributeZ;
}

std::vector<double> align_distribute_offsets(const std::vector<AxisExtent>& items, AlignOp op)
{
    std::vector<double> offsets(items.size(), 0.);
    if (items.empty())
        return offsets;
    auto center = [](const AxisExtent& e) { return 0.5 * (e.min + e.max); };

    if (is_distribute(op)) {
        if (items.size() < 3)
            return offsets;
        std::vector<size_t> order(items.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return center(items[a]) < center(items[b]); });
        const double first = center(items[order.front()]);
        const double step  = (center(items[order.back()]) - first) / double(items.size() - 1);
        for (size_t k = 1; k + 1 < order.size(); ++k)
            offsets[order[k]] = first + double(k) * step - center(items[order[k]]);
        return offsets;
    }

    double lo = items.front().min, hi = items.front().max;
    for (const AxisExtent& e : items) {
        lo = std::min(lo, e.min);
        hi = std::max(hi, e.max);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        switch (op) {
        case AlignOp::XMin:
        case AlignOp::YMin:
        case AlignOp::ZMin: offsets[i] = lo - items[i].min; break;
        case AlignOp::XMax:
        case AlignOp::YMax:
        case AlignOp::ZMax: offsets[i] = hi - items[i].max; break;
        default: offsets[i] = 0.5 * (lo + hi) - center(items[i]); break;
        }
    }
    return offsets;
}

namespace {

struct Item
{
    int           object_idx;
    int           instance_idx;
    int             volume_idx;
    const GLVolume* volume;
};

// Parts of one object instance are aligned among themselves; anything else is aligned as whole
// instances. A single full object counts as its parts only when it has one instance, so that
// selecting every instance of an object aligns the instances.
bool aligns_parts(const Selection& selection)
{
    if (selection.is_any_volume() || selection.is_any_modifier())
        return true;
    if (!selection.is_single_full_object())
        return false;
    const int obj_idx = selection.get_object_idx();
    const Model* model = selection.get_model();
    return obj_idx >= 0 && obj_idx < int(model->objects.size()) && model->objects[obj_idx]->instances.size() == 1;
}

std::vector<Item> collect_items(const Selection& selection, bool parts)
{
    std::vector<Item> items;
    const Model*      model = selection.get_model();
    if (parts) {
        for (unsigned int idx : selection.get_volume_idxs()) {
            const GLVolume* v = selection.get_volume(idx);
            if (v == nullptr || v->volume_idx() < 0 || v->object_idx() < 0 || v->object_idx() >= int(model->objects.size()))
                continue;
            items.push_back({v->object_idx(), v->instance_idx(), v->volume_idx(), v});
        }
        return items;
    }
    if (!selection.is_instance_mode())
        return items;
    for (const auto& [obj_idx, instances] : selection.get_content()) {
        if (obj_idx < 0 || obj_idx >= int(model->objects.size()))
            continue;
        const ModelObject* object = model->objects[obj_idx];
        for (int inst_idx : instances)
            if (inst_idx >= 0 && inst_idx < int(object->instances.size()))
                items.push_back({obj_idx, inst_idx, -1, nullptr});
    }
    return items;
}

bool same_plate(const std::vector<Item>& items)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    const int      first  = plates.find_instance(items.front().object_idx, items.front().instance_idx);
    return std::all_of(items.begin(), items.end(),
                       [&](const Item& it) { return plates.find_instance(it.object_idx, it.instance_idx) == first; });
}

} // namespace

bool can_align_distribute(GLCanvas3D& canvas, AlignOp op)
{
    const Selection& selection = canvas.get_selection();
    if (selection.is_empty() || selection.is_wipe_tower())
        return false;
    const bool parts = aligns_parts(selection);
    // Whole objects always rest on the bed, so aligning or spacing them in Z does nothing.
    if (!parts && align_op_info(op).axis == 2)
        return false;
    const std::vector<Item> items = collect_items(selection, parts);
    if (items.size() < (is_distribute(op) ? 3u : 2u))
        return false;
    // Aligning instances that sit on different plates would pull them onto one plate.
    return parts || same_plate(items);
}

bool align_distribute(GLCanvas3D& canvas, AlignOp op)
{
    if (!can_align_distribute(canvas, op))
        return false;
    Selection&              selection = canvas.get_selection();
    const bool              parts     = aligns_parts(selection);
    const std::vector<Item> items     = collect_items(selection, parts);
    const int               axis      = align_op_info(op).axis;

    std::vector<AxisExtent> extents;
    extents.reserve(items.size());
    for (const Item& it : items) {
        const BoundingBoxf3 box = it.volume ? it.volume->transformed_convex_hull_bounding_box() :
                                              selection.get_model()->objects[it.object_idx]->instance_bounding_box(size_t(it.instance_idx));
        extents.push_back({box.min[axis], box.max[axis]});
    }
    const std::vector<double> offsets = align_distribute_offsets(extents, op);

    selection.setup_cache();
    bool moved = false;
    for (size_t i = 0; i < items.size(); ++i) {
        if (std::abs(offsets[i]) < 1e-6)
            continue;
        Vec3d d = Vec3d::Zero();
        d[axis] = offsets[i];
        if (parts)
            selection.translate(items[i].object_idx, items[i].instance_idx, items[i].volume_idx, d, false);
        else
            selection.translate(items[i].object_idx, items[i].instance_idx, d);
        moved = true;
    }
    if (!moved)
        return false;
    // Parts are shared by all instances of their object; keep the other instances' copies in step.
    if (parts)
        selection.synchronize_unselected_volumes();
    // A single full object is selected in instance mode, so its parts need an explicit volume move.
    canvas.do_move(_u8L(align_op_info(op).label), parts);
    return true;
}

} // namespace GUI
} // namespace Slic3r
