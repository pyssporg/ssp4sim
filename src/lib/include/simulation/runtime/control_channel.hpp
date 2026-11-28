#pragma once

#include "runtime_control.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ssp4sim::runtime
{
    enum class ActionKind
    {
        Start,
        Pause,
        Resume,
        Stop,
        SetSpeed,
        SetOverride,
        ClearOverride,
        Snapshot,
    };

    enum class ActionStatus
    {
        Pending,
        Ok,
        Nok,
    };

    struct Action
    {
        uint64_t seq = 0;
        ActionKind kind{};
        uint64_t at_ns = 0;
        double speed = 0.0;
        std::string path;

        // SetOverride / ClearOverride target and coefficients.
        VariableSide side = VariableSide::Input;
        std::string model;
        std::string name;
        double k = 0.0;
        double m = 0.0;

        ActionStatus status = ActionStatus::Pending;
    };

    /// One immutable per-session snapshot; shared by status() copies.
    struct SessionIdentity
    {
        std::string session_id;
        std::vector<RecordedArtifact> artifacts;
    };

    struct ChannelStatus
    {
        RunState state = RunState::Created;
        uint64_t sim_time_ns = 0;
        double speed = 0.0;
        uint64_t applied_seq = 0;
        std::exception_ptr error;
        std::shared_ptr<const SessionIdentity> identity; // null before publish
    };

    /// Returned by wait_applied so status+error are coherent under one lock.
    struct AppliedAction
    {
        ActionStatus status = ActionStatus::Pending;
        std::exception_ptr error;
    };

    /// One mutex, one condition variable, one time-ordered action queue and the
    /// published status. The channel never transitions RunState; only the
    /// runtime writes it. Callers post actions and wait for their application.
    class ControlChannel
    {
    public:
        ControlChannel(double initial_speed, uint64_t start_time_ns);

        ControlChannel(const ControlChannel &) = delete;
        ControlChannel &operator=(const ControlChannel &) = delete;

        /// Assign a sequence number and queue the action by due time.
        /// Fire-and-forget: never blocks and keeps no completion record.
        uint64_t post(Action action);

        /// Queue the action and block until the runtime applies it. Holds the
        /// lock from enqueue to wakeup, so the completion cannot be missed and
        /// only awaited actions are recorded.
        AppliedAction post_and_wait(Action action);

        /// Coherent single-lock copy of state, time, speed, last error and
        /// identity.
        ChannelStatus status() const;

        /// Publish the run's stable session id and recorded artifacts. Both
        /// are copied into every subsequent status() snapshot.
        void publish_identity(std::string session_id,
                              std::vector<RecordedArtifact> artifacts);

        /// Number of queued actions not yet applied.
        std::size_t pending() const;

        /// Pop the earliest action whose due time is 0 or <= sim_time_ns.
        std::optional<Action> take_due(uint64_t sim_time_ns);

        /// Wait while not closed, no action is due at \p sim_time_ns, the run
        /// is still Running or Paused, and \p wall_deadline_ns has not passed.
        void wait_for_work(uint64_t sim_time_ns,
                           std::optional<uint64_t> wall_deadline_ns);

        /// Publish simulation time monotonically.
        void publish_time(uint64_t sim_time_ns);

        /// Publish the current speed.
        void set_speed(double speed);

        void set_state(RunState state);

        /// Record the last failed apply / run failure.
        void publish_error(std::exception_ptr error);

        /// Mark one applied action and advance applied_seq.
        void settle(uint64_t seq, ActionStatus status,
                    std::exception_ptr error = nullptr);

        /// Stop accepting actions and settle every queued action Nok.
        void close(std::exception_ptr error);

        /// Block until a terminal state is published.
        RunState wait_terminal();

        uint64_t applied_seq() const;

        bool is_closed() const;

    private:
        struct DoneAction
        {
            ActionStatus status = ActionStatus::Ok;
            std::exception_ptr error;
        };

        bool has_due_locked(uint64_t sim_time_ns) const;

        mutable std::mutex mutex_;
        std::condition_variable cv_;

        RunState state_ = RunState::Created;
        uint64_t sim_time_ns_ = 0;
        double speed_ = 0.0;

        uint64_t next_seq_ = 1;
        uint64_t applied_seq_ = 0;
        std::multimap<uint64_t, Action> queue_;
        std::set<uint64_t> awaited_;              // posted via post_and_wait
        std::map<uint64_t, DoneAction> done_;     // awaited actions only
        std::exception_ptr error_;
        std::shared_ptr<const SessionIdentity> identity_;

        bool closed_ = false;
    };
}
