#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ssp4sim
{
    /// Public lifecycle state. Mirrors ssp4sim_run_state.
    enum class RunState
    {
        Created = 0,
        Initialized = 1,
        Running = 2,
        Paused = 3,
        Stopping = 4,
        Completed = 5,
        Stopped = 6,
        Failed = 7,
    };

    /// Public terminal outcome of a run.
    enum class RunResult
    {
        Completed = 0,
        Stopped = 1,
        Failed = 2,
    };

    constexpr bool is_terminal(RunState state) noexcept
    {
        return state == RunState::Completed ||
               state == RunState::Stopped ||
               state == RunState::Failed;
    }

    /// Precondition: `state` is terminal.
    constexpr RunResult terminal_result(RunState state) noexcept
    {
        switch (state)
        {
        case RunState::Completed:
            return RunResult::Completed;
        case RunState::Stopped:
            return RunResult::Stopped;
        default:
            return RunResult::Failed;
        }
    }

    /// Coherent view of the run-control plane. This is the sole public
    /// readback of state, simulation time and speed.
    struct RunInfo
    {
        RunState state = RunState::Created;
        uint64_t sim_time_ns = 0;
        double speed = 0.0;
    };

    /// One recorded artifact belonging to a run.
    struct RecordedArtifact
    {
        std::string kind;    ///< "csv" or "sqlite"
        std::string path;    ///< absolute, lexically normalized
        int64_t run_id = -1; ///< SQLite internal run id; -1 for CSV
    };

    /// Immutable per-run identity: a stable session id and the artifacts this
    /// run records, in sink order. Valid after SimulationStepper::init().
    struct RunIdentity
    {
        std::string session_id;
        std::vector<RecordedArtifact> artifacts;
    };
}
