#pragma once

#include "control_channel.hpp"
#include "pacing_clock.hpp"
#include "runtime_control.hpp"
#include "shared_config.hpp"
#include "cpp_api/simulator_core.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

namespace ssp4sim
{
    /// Internal simulation side: owns the synchronous core, the run thread and
    /// the outer macro-step loop. Simulator delegates to it; ControlChannel is
    /// the single locked queue and status between caller and loop.
    namespace runtime
    {
        class SimulationRuntime
        {
        public:
            explicit SimulationRuntime(
                SharedConfig config,
                ssp4cpp::utils::log::Logger *log = nullptr);
            ~SimulationRuntime();

            SimulationRuntime(const SimulationRuntime &) = delete;
            SimulationRuntime &operator=(const SimulationRuntime &) = delete;
            SimulationRuntime(SimulationRuntime &&) = delete;
            SimulationRuntime &operator=(SimulationRuntime &&) = delete;

            void init();

            /// Spawn the run thread. One run per runtime; a second call
            /// throws. The caller posts the Start action through control().
            void start_thread();

            /// Wait for the run to finish; idempotent, rethrows on failure.
            RunResult wait();

            /// Join the run and return its failure text ("" if none). The
            /// caller posts Stop first when a consumer exists.
            std::string join() noexcept;

            /// Restore \p path into this runtime's freshly initialized core and
            /// publish the restored time to the control channel.
            void load_snapshot(const std::filesystem::path &path);

            /// The single shared control state; the runtime retains ownership.
            ControlChannel &control() noexcept;

            bool started() const;

            /// Resolve an override target against the initialized graph.
            /// Throws OverrideError("unknown_variable") when the model or
            /// variable is unknown. Returns whether the type can be scaled.
            bool validate_override(VariableSide side, const std::string &model,
                                   const std::string &name) const;

            /// Publish a speed update when no run thread consumes actions yet.
            void set_speed_before_start(double speed);

            /// Publish the pre-run cancellation (Stopped unless terminal).
            void cancel_before_start();

        private:
            RunResult run();
            bool apply(Action &action);
            std::optional<uint64_t> next_pacing_deadline() const;

            SharedConfig config_;
            ssp4cpp::utils::log::Logger *log_ = nullptr;
            std::unique_ptr<SimulatorCore> core_;

            std::unique_ptr<ControlChannel> channel_;
            PacingClock clock_;

            // Immutable after init(); read by callers, never by the run loop.
            std::map<std::tuple<VariableSide, std::string, std::string>, bool>
                override_targets_;

            mutable std::mutex lifecycle_mutex_;
            std::thread run_thread_;
            std::shared_future<RunResult> run_future_;
            bool run_started_ = false;
        };
    }
}
