#pragma once

#include "ssp4cpp/utils/log.hpp"

#include "ssp4sim_definitions.hpp"
#include "runtime_control.hpp"

#include "signal/storage.hpp"
#include "utils/fmi/fmu_info.hpp"
#include "../../1_ssp_parser/schema_extensions/parameter_value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <memory>

namespace ssp4sim::graph
{
    

    class ConnectorInfo : public types::IWritable
    {
    public:
        ConnectorInfo()
            : log(ssp4cpp::utils::log::make_logger("ssp4sim.model.ConnectorInfo"))
        {
        }

        types::DataType type;
        size_t size;
        std::string name;

        uint32_t index;
        uint64_t value_ref;

        handler::FmuInfo *fmu;
        signal::SignalStorage *storage;

        ssp4cpp::utils::log::Logger* log = nullptr;

        std::unique_ptr<ssp4sim::ext::ParameterValue> initial_value;

        /// Active affine override for this side's value, if any. Mutated only
        /// on the run thread at a macro-step boundary.
        std::optional<Override> override;

        bool forward_derivatives = false;
        int forward_derivatives_order = 0;

        std::string to_string() const override;

        static void set_start_values(std::unordered_map<std::string, ConnectorInfo> &connectors);

        static void set_initial_input_area(signal::SignalStorage *input_area,
                                           std::unordered_map<std::string, ConnectorInfo> &inputs,
                                           uint64_t time);

        static void write_data_to_model(std::unordered_map<std::string, ConnectorInfo> &inputs,
                                        signal::SignalStorage *storage,
                                        int area);

        static void read_values_from_model(std::unordered_map<std::string, ConnectorInfo> &outputs,
                                           signal::SignalStorage *storage,
                                           int area);

        /// Apply every active override in \p connectors to the values in
        /// \p area, in place. Real, Integer and Boolean are transformed;
        /// other types are left untouched.
        static void apply_overrides(std::unordered_map<std::string, ConnectorInfo> &connectors,
                                    signal::SignalStorage *storage,
                                    int area);

        static void apply_input_derivatives(std::unordered_map<std::string, ConnectorInfo> &inputs,
                                            std::size_t area);

        static void fetch_output_derivatives(std::unordered_map<std::string, ConnectorInfo> &outputs,
                                             std::size_t area);
    };
}
