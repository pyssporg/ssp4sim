#pragma once

#include "ssp4sim_definitions.hpp"

#include "invocable.hpp"

#include "model_connection.hpp"
#include "model_connector.hpp"

#include "signal/storage.hpp"

#include "simulation/serialize/serialize.hpp"

#include "shared_config.hpp"

#include "utils/fmi/fmu_info.hpp"

#include "ssp4cpp/utils/log.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

namespace ssp4sim::scheduling
{
    class DataAccessResolver;
}

namespace ssp4sim::graph
{

    class FmuModel final : public Invocable, public serialize::ISerialize
    {
    public:

        ssp4cpp::utils::log::Logger* log = nullptr;

        std::unique_ptr<handler::FmuInfo> fmu;

        std::unique_ptr<ssp4sim::signal::SignalStorage> input_area;
        std::unique_ptr<ssp4sim::signal::SignalStorage> output_area;

        std::unordered_map<std::string, ConnectorInfo> inputs;
        std::unordered_map<std::string, ConnectorInfo> outputs;
        std::unordered_map<std::string, ConnectorInfo> parameters;
        std::vector<ConnectionInfo> connections;

        // Read-path resolver, shared across models and set once at graph build.
        // Owned by the pipeline result; FmuModel only borrows it. Drives the copy in
        // pre()/direct_feedthrough(). Null ⇒ read path disabled (no copying).
        std::shared_ptr<ssp4sim::scheduling::DataAccessResolver> access_resolver = nullptr;

        bool forward_derivatives = false;
        bool canInterpolateInputs = false;
        size_t maxOutputDerivativeOrder = 0;
        bool fmu_logging = false;
        bool record_inputs = false;

        FmuModelConfig model_config;

        FmuModel(std::string name,
                 std::unique_ptr<ssp4sim::handler::FmuInfo> fmu,
                 size_t maxOutputDerivativeOrder,
                 ssp4sim::FmuModelConfig config);

        ~FmuModel();

        std::string to_string() const override;

        [[nodiscard]] bool has_feedthrough_outputs() const;
        [[nodiscard]] std::vector<ConnectionInfo*> feedthrough_connections();

        /// Find one input/output connector by variable name. Accepts the bare
        /// name (`y`) or the map-qualified name (`model.y`).
        ConnectorInfo *find_connector(VariableSide side,
                                      const std::string &name);
        const ConnectorInfo *find_connector(VariableSide side,
                                            const std::string &name) const;

        /// Strip this model's `<model>.` prefix from a connector map key.
        std::string bare_connector_name(
            std::string_view connector_name) const;

        void enter_init();

        void exit_init();

        uint64_t direct_feedthrough(uint64_t start);

        void pre(uint64_t step_start, uint64_t step_end);

        void post(uint64_t time);

        uint64_t step(StepData step_data);

        void serialize(serialize::Writer &out) const override;
        void deserialize(serialize::Reader &in) override;

        uint64_t invoke(StepData step_data) override final;
    };
}
