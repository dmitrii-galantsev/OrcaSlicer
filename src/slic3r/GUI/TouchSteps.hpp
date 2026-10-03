#ifndef slic3r_GUI_TouchSteps_hpp_
#define slic3r_GUI_TouchSteps_hpp_

#include <string>

namespace Slic3r {

class ConfigOptionDef;

namespace GUI {

// How much one press of a touch − / + button changes an option. opt_key may carry a "#<extruder>"
// suffix. value is the number currently shown; percent is true when it is shown with a '%'.
double touch_step_for_option(const ConfigOptionDef& def, const std::string& opt_key, double value, bool percent);

// value moved one step in direction (+1 / -1), snapped to the multiples of step so that 203 + 5
// gives 205, then clamped to [min, max].
double touch_step_value(double value, double step, int direction, double min, double max);

// Decimal places needed to show multiples of step exactly.
int touch_step_decimals(double step);

}} // namespace Slic3r::GUI

#endif
