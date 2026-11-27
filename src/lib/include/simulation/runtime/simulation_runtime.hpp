#pragma once

#include "control_channel.hpp"
#include "pacing_clock.hpp"
#include "runtime_control.hpp"
#include "shared_config.hpp"
#include "cpp_api/simulator_core.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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

            /// Spawn the run thread, post Start and wait until it is applied.
            /// One run per runtime; a second call throws.
            void start();

            /// start() followed by wait().
            void simulate();

            void pause(std::optional<uint64_t> at_sim_time_ns = std::nullopt);
            void resume();
            void stop(std::optional<uint64_t> at_sim_time_ns = std::nullopt);
            void set_speed(double speed,
                           std::optional<uint64_t> at_sim_time_ns = std::nullopt);

            RunInfo info() const;

            /// Blocking snapshot of a Running or Paused run. Returns true on
            /// success, false outside Running/Paused or when the save fails.
            bool snapshot(const std::filesystem::path &path);

            /// Wait for the run to finish; idempotent, rethrows on failure.
            RunResult wait();

            /// Unconditional teardown; never throws. Returns the run's failure
            /// text ("" if none).
            std::string shutdown() noexcept;

            /// Restore \p path into this runtime's freshly initialized core and
            /// publish the restored time to the control channel.
            void load_snapshot(const std::filesystem::path &path);

        private:
            RunResult run();
            bool apply(Action &action);
            void submit_and_wait(Action action);
            std::optional<uint64_t> next_pacing_deadline() const;
            bool has_run_thread();

            SharedConfig config_;
            ssp4cpp::utils::log::Logger *log_ = nullptr;
            std::unique_ptr<SimulatorCore> core_;

            std::unique_ptr<ControlChannel> channel_;
            PacingClock clock_;

            std::mutex lifecycle_mutex_;
            std::thread run_thread_;
            std::shared_future<RunResult> run_future_;
            bool run_started_ = false;
        };
    }
}
