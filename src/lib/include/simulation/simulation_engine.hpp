#pragma once

#include "runtime_control.hpp"
#include "shared_config.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace ssp4sim
{
    /**
     * @brief Owns the simulation engine and executes one run.
     *
     * Setup (SSP import, graph construction, recorder wiring) and the run body
     * live here. `run()` is the in-thread execution body: it is called once by
     * the thread that owns the run and is the only method that mutates
     * execution state. The control and query methods are safe to call from
     * another thread because they go through `RunControl`.
     *
     * The owner must join the run before destroying the engine.
     */
    class SimulationEngine
    {
    public:
        explicit SimulationEngine(SharedConfig config,
                                  ssp4cpp::utils::log::Logger *log = nullptr);
        ~SimulationEngine();

        SimulationEngine(const SimulationEngine &) = delete;
        SimulationEngine &operator=(const SimulationEngine &) = delete;

        SimulationEngine(SimulationEngine &&) = delete;
        SimulationEngine &operator=(SimulationEngine &&) = delete;

        /// Build the graph, executors and recorder. Throws on a second call.
        void init();

        bool initialized() const noexcept;

        /// Execute the simulation on the calling thread. Returns the terminal
        /// result and rethrows simulation failures after flushing the recorder.
        /// A pre-run teardown makes this return Stopped without starting the
        /// recorder or invoking the graph.
        RunResult run();

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
        RunState state() const;
        uint64_t sim_time_ns() const;
        double speed() const;

        /// Unconditional teardown; never throws. Pre-run cancellation publishes
        /// Stopped so the run thread and start barrier cannot hang.
        void request_stop();

        /// Block until the run leaves Initialized (Running or terminal).
        void wait_until_started();

    private:
        struct Impl;
        std::unique_ptr<Impl> p;
    };
}
