#pragma once

#include "runtime_control.hpp"
#include "shared_config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ssp4sim
{
    /// Failure raised by snapshot save/load. `code` is one of "unsupported",
    /// "io_error", "invalid_snapshot" or "apply_failed".
    struct SnapshotError : std::runtime_error
    {
        std::string code;

        SnapshotError(std::string code, const std::string &message);
    };

    /// Failure raised while resolving a runtime override target. `code` is one
    /// of "unknown_variable" or "unsupported_type".
    struct OverrideError : std::runtime_error
    {
        std::string code;

        OverrideError(std::string code, const std::string &message);
    };

    /// One overridable variable discovered from the initialized graph.
    struct OverrideTarget
    {
        VariableSide side = VariableSide::Input;
        std::string model;
        std::string name;
        bool scalable = false;
    };

    /**
     * @brief Synchronous simulation core: setup, graph, executors, recorder and
     * one macro step.
     *
     * This class owns everything needed to advance simulation time and nothing
     * that belongs to the asynchronous lifecycle: no worker thread, no
     * condition variable, no wall-clock pacing, no RunState and no command
     * queue. Threading, pacing and control live in Simulator.
     *
     * The core is single-threaded and not thread-safe. All methods must be
     * called from the same thread, and no other thread may call it while a
     * step is in progress.
     */
    class SimulatorCore
    {
    public:
        explicit SimulatorCore(SharedConfig config,
                               ssp4cpp::utils::log::Logger *log = nullptr);
        ~SimulatorCore();

        SimulatorCore(const SimulatorCore &) = delete;
        SimulatorCore &operator=(const SimulatorCore &) = delete;

        /// Build the graph and executors, then initialize the graph and
        /// recorder. Throws on a second call.
        void init();

        bool initialized() const noexcept;

        /// Stable session id for this run. Valid after init().
        const std::string &session_id() const noexcept;

        /// Artifacts this run records, in sink order. Valid after init().
        const std::vector<RecordedArtifact> &artifacts() const noexcept;

        /// Validate the timestep and start the recorder.
        void start();

        /// Advance one configured macro step. Returns true when the run has
        /// reached the configured end time, false when it advanced normally.
        /// Throws std::logic_error before start() or after finish().
        bool step();

        /// Advance to an exact simulation time, using a partial final step.
        /// Clamps the target to end_time and never sleeps. Returns true when the
        /// run has reached the configured end time.
        bool advance_to(uint64_t target_ns);

        uint64_t sim_time_ns() const noexcept;
        uint64_t end_time_ns() const noexcept;

        /// Write a self-describing snapshot of the full simulation state.
        /// Legal mid-run; the caller guarantees no concurrent step(). Returns
        /// true on success; throws std::logic_error before init() and
        /// SnapshotError otherwise.
        bool save_snapshot(const std::filesystem::path &path);

        /// Restore a snapshot into a freshly initialized core. Requires
        /// init() and no start()/finish(). Throws std::logic_error otherwise.
        /// The file is validated completely before any live state changes.
        /// On any failure this throws and leaves the core unspecified; the
        /// caller must destroy it and build a fresh one.
        void load_snapshot(const std::filesystem::path &path);

        /// Variables exposed by the initialized graph, for override target
        /// validation. Valid after init().
        std::vector<OverrideTarget> override_targets() const;

        /// Set or clear the effective override for one variable. Must be
        /// called on the run thread at a macro-step boundary, never while a
        /// step is in progress.
        void apply_override(VariableSide side, const std::string &model,
                            const std::string &name, double k, double m);
        void clear_override(VariableSide side, const std::string &model,
                            const std::string &name);

        /// Stop and flush the recorder. Idempotent; propagates recorder
        /// failures.
        void finish();

    private:
        void log_model_walltimes() const;

        struct Impl;
        std::unique_ptr<Impl> p;
    };
}
