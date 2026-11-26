#pragma once

#include "shared_config.hpp"
#include "runtime_control.hpp"

#include <cstdint>
#include <memory>

namespace ssp4sim
{
    /// Outcome of one synchronous macro step.
    enum class StepStatus
    {
        Advanced,
        Completed,
    };

    /**
     * @brief Synchronous simulation core: setup, graph, executors, recorder and
     * one macro step.
     *
     * This class owns everything needed to advance simulation time and nothing
     * that belongs to the asynchronous lifecycle: no worker thread, no
     * condition variable, no wall-clock pacing, no RunState and no command
     * queue. Threading, pacing and control live in ThreadedRunner.
     *
     * The stepper is single-threaded and not thread-safe. All methods must be
     * called from the same thread, and no other thread may call it while a
     * step is in progress.
     */
    class SimulationStepper
    {
    public:
        explicit SimulationStepper(SharedConfig config,
                                   ssp4cpp::utils::log::Logger *log = nullptr);
        ~SimulationStepper();

        SimulationStepper(const SimulationStepper &) = delete;
        SimulationStepper &operator=(const SimulationStepper &) = delete;

        /// Build the graph and executors, then initialize the graph and
        /// recorder. Throws on a second call.
        void init();

        bool initialized() const noexcept;

        /// Immutable identity of this run: stable session id and recorded
        /// artifacts. Valid after init().
        RunIdentity run_identity() const;

        /// Validate the timestep and start the recorder.
        void start();

        /// Advance one configured macro slice. Throws std::logic_error before
        /// start() or after finish().
        StepStatus step();

        /// Advance to an exact simulation time, using a partial final slice.
        /// Clamps the target to end_time and never sleeps.
        StepStatus advance_to(uint64_t target_ns);

        uint64_t sim_time_ns() const noexcept;
        uint64_t end_time_ns() const noexcept;

        /// Stop and flush the recorder. Idempotent; propagates recorder
        /// failures.
        void finish();

    private:
        void log_model_walltimes() const;

        struct Impl;
        std::unique_ptr<Impl> p;
    };
}
