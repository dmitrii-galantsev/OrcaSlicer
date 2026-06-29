#ifndef VORTEK_GCODE_HPP
#define VORTEK_GCODE_HPP

#include "GCode.hpp"

namespace Slic3r {
    class GCode;
    class DynamicConfig;
    class PlaceholderParser;
    struct FullPrintConfig;
    class Print;
}

namespace Vortek {

/**
 * @brief Delegate hook functions for patching parameters and configs inside Slic3r::GCode export pipeline.
 */
namespace GCodeHooks {

/**
 * @brief Hook called during layer changes to update configuration parameters (temperatures, retracts) for the active nozzle.
 * 
 * @param gcode Reference to the GCode processor object
 * @param layer_id 0-based layer index
 */
void update_layer_related_config(Slic3r::GCode& gcode, int layer_id);

/**
 * @brief Hook called during toolchange (T0/T1) to override dynamic parameters in the G-code output.
 * 
 * If a nozzle switch is occurring on the H2C printhead, this updates speeds, retract lengths, and temperatures dynamically.
 * 
 * @param gcode Reference to the GCode processor object
 * @param dyn_config Reference to the dynamic configuration being sent to the G-code generator
 * @param new_filament_id The logical filament ID we are switching to
 * @param layer_id 0-based layer index
 */
void patch_toolchange_dyn_config(
    Slic3r::GCode& gcode,
    Slic3r::DynamicConfig& dyn_config,
    int new_filament_id,
    int layer_id);

/**
 * @brief Registers all H2C/BBL-specific placeholder variables into a PlaceholderParser instance.
 *
 * Must be called on m_placeholder_parser_integration.parser AFTER it is initialized from
 * print.placeholder_parser() in do_export(), because that assignment overwrites any earlier
 * registrations made into GCode::m_placeholder_parser.
 *
 * @param parser  The integration parser (m_placeholder_parser_integration.parser)
 * @param config  The full print config (GCode::m_config)
 * @param print   Pointer to the Print object (may be nullptr)
 */
void register_vortek_placeholders(
    Slic3r::PlaceholderParser& parser,
    const Slic3r::FullPrintConfig& config,
    const Slic3r::Print* print);



} // namespace GCodeHooks
} // namespace Vortek

#endif // VORTEK_GCODE_HPP
