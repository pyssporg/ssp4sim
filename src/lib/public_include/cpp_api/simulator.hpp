#pragma once

#include "runtime_control.hpp"
#include "shared_config.hpp"
#include "simulator_core.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>


namespace ssp4sim
{
    struct SimulatorPrivate;

    /**
     * @brief Caller-facing facade for one simulation session.
     *
     * Simulator owns only the creation config and an internal
     * runtime::SimulationRuntime. The runtime owns the synchronous
     * SimulatorCore, the run thread and the outer macro-step loop; the control
     * channel is the barrier between the caller and the loop. Start-up, queries
     * and control are owned directly. info() is the sole public readback of run
     * state, simulation time and speed. snapshot() is one blocking action on
     * the control channel; no snapshot service or correlation state lives in
     * the library.
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

        /// Spawn the run thread and wait until the control channel leaves
        /// Initialized (Running or terminal). One run per Simulator; a second
        /// call throws.
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

        /// Unconditional teardown; never throws. Running/Paused -> Stopping,
        /// Stopping stays Stopping, Created/Initialized -> Stopped (pre-run
        /// cancellation), terminal states are left untouched.
        void shutdown() noexcept;

        /// Set the wall-clock pace (finite, >= 0). 0 runs as fast as possible.
        /// `at_sim_time_ns` defers the change to that simulation time.
        void set_speed(double speed,
                       std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Sole public readback of run state, simulation time and speed, all
        /// read together so the three values agree.
        RunInfo info() const;

        /// Blocking snapshot of a Running or Paused run. Returns true when the
        /// file was written, false outside Running/Paused or when the save
        /// fails; the failure reason is logged.
        bool snapshot(const std::filesystem::path &path);

        /// Blocking restore of \p path into a freshly initialized run. Prior
        /// control states are stopped and joined first; the new run keeps the
        /// current pace. After this call the Simulator is Initialized (or
        /// Running when \p start is true): a subsequent init() is invalid.
        void restore(const std::filesystem::path &path, bool start = true);

        /// Stop and join any current run, discard all per-run state, and
        /// rebuild a fresh initialized run from the creation config. Leaves the
        /// Simulator Initialized at the configured start time; a subsequent
        /// init() is invalid. Throws if construction or initialization fails.
        void reset();

        struct RebuildResult;

        /// Quiesce \p old (stop/join when it had a run), then build a fresh
        /// simulator from \p config. A null \p snapshot builds an initialized
        /// simulator (reset); a non-null path builds a restored one (restore,
        /// carrying the old run's pace and rotating the CSV segment). Never
        /// throws; failures land in the result.
        static RebuildResult rebuild(Simulator *old, SharedConfig config,
                                     const std::filesystem::path *snapshot);

        /// Wait for the run to finish; idempotent, rethrows on failure.
        RunResult wait();

    private:
        /// Construct, init(), and (for restored) load a snapshot. Used by
        /// rebuild(); no threads or run control are involved.
        static std::unique_ptr<Simulator> create_initialized(SharedConfig config);
        static std::unique_ptr<Simulator> create_restored(
            SharedConfig config, const std::filesystem::path &path);

        static SharedConfig prepare_restore_config(SharedConfig config,
                                                   double prior_speed);

        std::unique_ptr<SimulatorPrivate> p;
    };

    struct Simulator::RebuildResult
    {
        std::unique_ptr<Simulator> replacement;  // null on failure
        std::exception_ptr exception;             // preserves the original type
        std::string message;                      // human message (empty on success)
        RunInfo old_info{};
        std::string old_run_error;
        bool had_run = false;
    };
}
