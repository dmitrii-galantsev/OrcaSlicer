#ifndef VORTEK_PRINT_HOOKS_HPP
#define VORTEK_PRINT_HOOKS_HPP

#include <vector>
#include <memory>

namespace Slic3r {
    class Print;
    class PrintConfigDef;
    namespace MultiNozzleUtils {
        class NozzleGroupResultBase;
    }
}

namespace Vortek {

/**
 * @brief Delegate hooks for modifying configuration values on Slic3r::Print.
 * 
 * Contains helpers for remapping physical extruders, overriding retraction distances, 
 * temperatures, and registering configuration schemas.
 */
class PrintHooks {
public:
    /**
     * @brief Registers the 18 custom Vortek H2C-specific parameters into the slicer's global configuration registry.
     * 
     * @param def Pointer to the PrintConfigDef object containing the configuration definition
     */
    static void init_vortek_params(Slic3r::PrintConfigDef* def);

    /**
     * @brief Remaps and trims logical filament configurations to align with physical active nozzles.
     * 
     * @param print Reference to the Print object
     * @param f_maps Extruder mapping array
     * @param f_volume_maps Volume/Nozzle type mapping array
     * @param f_nozzle_maps Physical nozzle slot index mapping array
     */
    static void update_filament_maps_to_config(
        Slic3r::Print& print,
        const std::vector<int>& f_maps,
        const std::vector<int>& f_volume_maps,
        const std::vector<int>& f_nozzle_maps
    );

    /**
     * @brief Evaluates the nozzle group result and updates all active parameters (diameters, retracts, flow caps) on the Print config.
     * 
     * @param print Reference to the Print object
     * @param group_result Reference to the resolved nozzle grouping result
     */
    static void update_to_config_by_nozzle_group_result(
        Slic3r::Print& print,
        const Slic3r::MultiNozzleUtils::NozzleGroupResultBase& group_result
    );
};

} // namespace Vortek

#endif // VORTEK_PRINT_HOOKS_HPP
