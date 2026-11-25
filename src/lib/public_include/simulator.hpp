#pragma once

#include "runtime_control.hpp"
#include "shared_config.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>


namespace ssp4sim
{
    struct SimulatorPrivate;

    /**
     * @brief A high-level simulator class that simplifies the process of running
     * a simulation.
     *
     * This class is the public lifecycle facade. It owns the internal run
     * thread and a SimulationEngine. Setup, queries and control are forwarded
     * to the engine; the run body itself executes in SimulationEngine::run()
     * on the run thread. info() is the sole public readback of run state,
     * simulation time and speed.
     */


    class Simulator
    {
    public:

        /**
         * @brief Constructs a new Simulator object.
         *
         * @param config_path The path to the simulation configuration file.
         */
        explicit Simulator(const std::string config_path);

        /**
         * @brief Constructs a new Simulator object from an already-built config.
         *
         * @param config The shared configuration (moved into the simulator).
         * @param log Optional logger; falls back to `config.log` when null.
         */
        explicit Simulator(SharedConfig config,
                           ssp4cpp::utils::log::Logger *log = nullptr);

        Simulator(const Simulator &) = delete;
        Simulator &operator=(const Simulator &) = delete;

        Simulator(Simulator &&) = delete;
        Simulator &operator=(Simulator &&) = delete;

        ~Simulator();

        /**
         * @brief Initializes the simulator.
         */
        void init();

        /**
         * @brief Runs the simulation.
         */
        void simulate();

        /// Spawn the run thread and wait until RunControl leaves Initialized
        /// (Running or terminal). One run per Simulator; a second call throws.
        void start();

        /// Pause a Running run. Invalid state throws std::logic_error.
        /// `at_sim_time_ns` defers the pause to that simulation time.
        void pause(std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Resume a Paused run. Invalid state throws std::logic_error.
        void resume();

        /// Stop a Running or Paused run. Idempotent once Stopping or terminal;
        /// pre-run states throw as before. Partial results are flushed.
        /// `at_sim_time_ns` defers the stop to that simulation time.
        void stop(std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Set the wall-clock pace (finite, >= 0). 0 runs as fast as possible.
        /// `at_sim_time_ns` defers the change to that simulation time.
        void set_speed(double speed,
                       std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Sole public readback of run state, simulation time and speed, all
        /// read together so the three values agree.
        RunInfo info() const;

        /// Block until the run leaves Initialized (Running or terminal). Used
        /// as the start barrier so a start->stop sequence cannot observe a
        /// pre-run state.
        void wait_until_started();

        /// Wait for the run to finish; idempotent, rethrows on failure.
        RunResult wait();

    private:
        std::unique_ptr<SimulatorPrivate> p;
    };
}
