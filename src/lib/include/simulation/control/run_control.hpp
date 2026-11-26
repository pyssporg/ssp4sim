#pragma once

#include "run_state.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace ssp4sim::control
{
    /// Single owner of the run-control state machine.
    ///
    /// All mutable state (state, simulation time, speed, pacing anchors and the
    /// deferred command queue) is guarded by one mutex. Immediate operations
    /// update it under the lock; deferred operations live in one ordered
    /// container and are applied only by the run thread inside before_slice()
    /// or by publish_boundary(), in (time, insertion) order.
    ///
    /// Terminal states (Completed, Stopped, Failed) are monotonic: no later
    /// request_stop(), stop() or finish() may overwrite them. A pre-run
    /// teardown (request_stop() while Created/Initialized) cancels the run and
    /// publishes Stopped.
    class RunControl
    {
    public:
        RunControl(double initial_speed, uint64_t start_time_ns);

        RunControl(const RunControl &) = delete;
        RunControl &operator=(const RunControl &) = delete;

        void pause(std::optional<uint64_t> at_sim_time_ns = std::nullopt);
        void resume();
        void stop(std::optional<uint64_t> at_sim_time_ns = std::nullopt);
        void set_speed(double speed,
                       std::optional<uint64_t> at_sim_time_ns = std::nullopt);

        /// Unconditional teardown; never throws. Running/Paused -> Stopping,
        /// Stopping stays Stopping, Created/Initialized -> Stopped (pre-run
        /// cancellation), terminal states are left untouched.
        void request_stop();

        /// The single control entry point, called once per macro step by the
        /// run thread. Applies every due deferred command at the loop top,
        /// including while paused, then performs the pause/speed pacing wait,
        /// re-checking for new commands on every wakeup. Returns true when the
        /// slice may run, false when the run must stop.
        bool before_slice(uint64_t slice_start_ns);

        /// Publish a macro boundary without pacing. Ignores terminal states.
        /// Simulation time is monotonic, even while Stopping; due commands are
        /// applied only when not Stopping.
        void publish_boundary(uint64_t boundary_ns);

        // Created -> Initialized after a successful Simulator::init().
        void mark_initialized();

        /// Start the run. Returns false when a pre-run teardown has already
        /// cancelled the run (Stopped/Stopping); throws when the run has
        /// already started or reached a terminal state.
        bool begin_run();

        /// Publish the terminal state and return the effective result. If the
        /// run is already terminal, the state is left untouched and its result
        /// is returned.
        RunResult finish(RunResult result);

        /// Block until the run leaves Initialized (Running or terminal). Used
        /// as the start barrier; pre-run failures publish a terminal state so
        /// this cannot hang.
        void wait_until_started();

        RunInfo info() const;
        RunState state() const;
        uint64_t sim_time() const;
        double speed() const;

        static uint64_t compute_deadline_ns(int64_t anchor_wall_ns,
                                            uint64_t anchor_sim_ns,
                                            uint64_t sim_time_ns,
                                            double speed);

    private:
        struct Command
        {
            enum class Kind
            {
                Pause,
                Stop,
                Speed,
            };

            Kind kind;
            double speed = 0.0;
        };

        static bool can_control(RunState state) noexcept;

        void enqueue_locked(uint64_t at_sim_time_ns, Command command);
        bool has_due_locked(uint64_t boundary_time_ns) const;
        void apply_due_locked(uint64_t boundary_time_ns);
        void apply_command_locked(const Command &command);
        void reanchor_locked(uint64_t sim_time_ns);
        void changed_locked();

        void apply_pause_locked();
        void apply_stop_locked();
        void apply_speed_locked(double speed);

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::multimap<uint64_t, Command> deferred_;
        int64_t anchor_wall_ns_ = 0;
        uint64_t anchor_sim_ns_ = 0;

        RunState state_ = RunState::Created;
        uint64_t sim_time_ns_ = 0;
        double speed_ = 0.0;
    };
}
