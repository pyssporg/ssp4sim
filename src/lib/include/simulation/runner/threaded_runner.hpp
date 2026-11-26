#pragma once

#include "runtime_control.hpp"
#include "shared_config.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace ssp4sim
{
    /**
     * @brief Asynchronous simulation layer: worker thread, command queue,
     * pacing, RunState and the terminal result.
     *
     * ThreadedRunner owns the only async machinery in the simulation stack.
     * It drives a synchronous SimulationStepper from a worker thread and
     * routes all control through RunControl. Simulator is a thin compatibility
     * facade over this class.
     */
    class ThreadedRunner
    {
    public:
        explicit ThreadedRunner(SharedConfig config,
                                ssp4cpp::utils::log::Logger *log = nullptr);
        ~ThreadedRunner();

        ThreadedRunner(const ThreadedRunner &) = delete;
        ThreadedRunner &operator=(const ThreadedRunner &) = delete;

        /// Build the stepper state. Throws on a second call.
        void init();

        bool initialized() const noexcept;

        /// Spawn the run thread and wait until RunControl leaves Initialized
        /// (Running or terminal). One run per runner; a second call throws.
        void start();

        /// Wait for the run to finish; idempotent, rethrows on failure.
        RunResult wait();

        /// Pause a Running run. Invalid state throws std::logic_error.
        void pause(std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Resume a Paused run. Invalid state throws std::logic_error.
        void resume();

        /// Stop a Running or Paused run. Idempotent once Stopping or terminal;
        /// pre-run states throw as before. Partial results are flushed.
        void stop(std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Set the wall-clock pace (finite, >= 0). 0 runs as fast as possible.
        void set_speed(double speed,
                       std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Coherent read of state, simulation time and speed.
        RunInfo info() const;

        /// Immutable per-run identity: stable session id and recorded
        /// artifacts. Valid after init().
        RunIdentity run_identity() const;

        /// Unconditional teardown; never throws. Pre-run cancellation publishes
        /// Stopped so the run thread and start barrier cannot hang.
        void request_stop();

        /// Block until the run leaves Initialized (Running or terminal).
        void wait_until_started();

        /// Execute the run body on the calling thread. Internal/test entry
        /// point; start() is the normal asynchronous entry.
        RunResult run();

    private:
        struct Impl;
        std::unique_ptr<Impl> p;
    };
}
