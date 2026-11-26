#include "simulation/control/run_control.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ssp4sim::control
{
    namespace
    {
        int64_t steady_now_ns()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    RunControl::RunControl(double initial_speed, uint64_t start_time_ns)
    {
        if (!std::isfinite(initial_speed) || initial_speed < 0.0)
        {
            throw std::invalid_argument(
                "RunControl: initial speed must be finite and non-negative");
        }

        speed_ = initial_speed;
        sim_time_ns_ = start_time_ns;
        anchor_sim_ns_ = start_time_ns;
        anchor_wall_ns_ = steady_now_ns();
    }

    bool RunControl::can_control(RunState state) noexcept
    {
        return state == RunState::Initialized ||
               state == RunState::Running ||
               state == RunState::Paused;
    }

    void RunControl::changed_locked()
    {
        cv_.notify_all();
    }

    void RunControl::enqueue_locked(uint64_t at_sim_time_ns, Command command)
    {
        deferred_.emplace(at_sim_time_ns, command);
        changed_locked();
    }

    bool RunControl::has_due_locked(uint64_t boundary_time_ns) const
    {
        return !deferred_.empty() &&
               deferred_.begin()->first <= boundary_time_ns;
    }

    void RunControl::apply_due_locked(uint64_t boundary_time_ns)
    {
        auto it = deferred_.begin();
        while (it != deferred_.end() && it->first <= boundary_time_ns)
        {
            const Command command = it->second;
            it = deferred_.erase(it);
            apply_command_locked(command);
        }
    }

    void RunControl::apply_command_locked(const Command &command)
    {
        switch (command.kind)
        {
        case Command::Kind::Pause:
            apply_pause_locked();
            break;
        case Command::Kind::Stop:
            apply_stop_locked();
            break;
        case Command::Kind::Speed:
            apply_speed_locked(command.speed);
            break;
        }
    }

    void RunControl::reanchor_locked(uint64_t sim_time_ns)
    {
        anchor_wall_ns_ = steady_now_ns();
        anchor_sim_ns_ = sim_time_ns;
    }

    void RunControl::apply_pause_locked()
    {
        if (state_ == RunState::Running)
        {
            state_ = RunState::Paused;
        }
    }

    void RunControl::apply_stop_locked()
    {
        if (state_ == RunState::Running || state_ == RunState::Paused)
        {
            state_ = RunState::Stopping;
        }
    }

    void RunControl::apply_speed_locked(double speed)
    {
        if (can_control(state_))
        {
            speed_ = speed;
            reanchor_locked(sim_time_ns_);
        }
    }

    void RunControl::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != RunState::Running)
        {
            throw std::logic_error("RunControl::pause() requires a Running simulation");
        }

        if (at_sim_time_ns)
        {
            enqueue_locked(*at_sim_time_ns, Command{Command::Kind::Pause});
        }
        else
        {
            apply_pause_locked();
            changed_locked();
        }
    }

    void RunControl::resume()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != RunState::Paused)
        {
            throw std::logic_error("RunControl::resume() requires a Paused simulation");
        }

        state_ = RunState::Running;
        reanchor_locked(sim_time_ns_);
        changed_locked();
    }

    void RunControl::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const RunState current = state_;

        // Idempotent for a run that is already stopping or terminal.
        if (current == RunState::Stopping || is_terminal(current))
        {
            return;
        }

        if (current != RunState::Running && current != RunState::Paused)
        {
            throw std::logic_error(
                "RunControl::stop() requires a Running or Paused simulation");
        }

        if (at_sim_time_ns)
        {
            enqueue_locked(*at_sim_time_ns, Command{Command::Kind::Stop});
        }
        else
        {
            apply_stop_locked();
            changed_locked();
        }
    }

    void RunControl::set_speed(double speed,
                               std::optional<uint64_t> at_sim_time_ns)
    {
        if (!std::isfinite(speed) || speed < 0.0)
        {
            throw std::invalid_argument(
                "RunControl::set_speed() requires a finite speed >= 0");
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (!can_control(state_))
        {
            throw std::logic_error(
                "RunControl::set_speed() requires an Initialized, Running or Paused simulation");
        }

        if (at_sim_time_ns)
        {
            enqueue_locked(*at_sim_time_ns,
                           Command{Command::Kind::Speed, speed});
        }
        else
        {
            apply_speed_locked(speed);
            changed_locked();
        }
    }

    void RunControl::request_stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        switch (state_)
        {
        case RunState::Running:
        case RunState::Paused:
            apply_stop_locked();
            break;
        case RunState::Stopping:
            break;
        case RunState::Created:
        case RunState::Initialized:
            state_ = RunState::Stopped;
            break;
        default:
            return;
        }

        deferred_.clear();
        changed_locked();
    }

    bool RunControl::before_slice(uint64_t slice_start_ns)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (slice_start_ns > sim_time_ns_)
        {
            sim_time_ns_ = slice_start_ns;
        }

        for (;;)
        {
            // Apply due commands at the loop top, including while paused.
            apply_due_locked(sim_time_ns_);

            if (state_ == RunState::Stopping || state_ == RunState::Stopped)
            {
                return false;
            }

            if (state_ == RunState::Paused)
            {
                cv_.wait(lock, [this]()
                         {
                             return state_ != RunState::Paused ||
                                    has_due_locked(sim_time_ns_);
                         });
                continue;
            }

            if (state_ != RunState::Running)
            {
                return false;
            }

            const double observed_speed = speed_;
            if (observed_speed == 0.0)
            {
                return true;
            }

            const uint64_t deadline_ns =
                compute_deadline_ns(anchor_wall_ns_, anchor_sim_ns_,
                                    slice_start_ns, observed_speed);
            if (deadline_ns == 0)
            {
                return true;
            }

            const auto deadline = std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(static_cast<int64_t>(deadline_ns)));

            const bool interrupted = cv_.wait_until(
                lock, deadline,
                [this, observed_speed]()
                {
                    return state_ != RunState::Running ||
                           speed_ != observed_speed ||
                           has_due_locked(sim_time_ns_);
                });

            if (!interrupted)
            {
                return true;
            }

            // A speed change reanchors inside apply_speed_locked(); loop back
            // to re-evaluate state and the next deadline.
        }
    }

    void RunControl::publish_boundary(uint64_t boundary_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (is_terminal(state_))
        {
            return;
        }

        if (boundary_ns > sim_time_ns_)
        {
            sim_time_ns_ = boundary_ns;
        }

        if (state_ != RunState::Stopping)
        {
            apply_due_locked(boundary_ns);
        }

        changed_locked();
    }

    void RunControl::mark_initialized()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == RunState::Created)
        {
            state_ = RunState::Initialized;
            changed_locked();
        }
    }

    bool RunControl::begin_run()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ == RunState::Stopped || state_ == RunState::Stopping)
        {
            // Pre-run teardown cancelled the run.
            return false;
        }

        if (state_ != RunState::Created && state_ != RunState::Initialized)
        {
            throw std::logic_error("RunControl::begin_run() called more than once");
        }

        state_ = RunState::Running;
        reanchor_locked(sim_time_ns_);
        changed_locked();
        return true;
    }

    RunResult RunControl::finish(RunResult result)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (is_terminal(state_))
        {
            return terminal_result(state_);
        }

        switch (result)
        {
        case RunResult::Completed:
            // A stop request that arrived while the run was unwinding must
            // still publish Stopped, not Completed.
            state_ = (state_ == RunState::Stopping)
                         ? RunState::Stopped
                         : RunState::Completed;
            break;
        case RunResult::Stopped:
            state_ = RunState::Stopped;
            break;
        case RunResult::Failed:
            state_ = RunState::Failed;
            break;
        }

        deferred_.clear();
        changed_locked();
        return terminal_result(state_);
    }

    void RunControl::wait_until_started()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return state_ != RunState::Initialized; });
    }

    RunInfo RunControl::info() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return RunInfo{state_, sim_time_ns_, speed_};
    }

    RunState RunControl::state() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    uint64_t RunControl::sim_time() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sim_time_ns_;
    }

    double RunControl::speed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return speed_;
    }

    uint64_t RunControl::compute_deadline_ns(int64_t anchor_wall_ns,
                                             uint64_t anchor_sim_ns,
                                             uint64_t sim_time_ns,
                                             double speed)
    {
        if (!(speed > 0.0) || !std::isfinite(speed))
        {
            return 0;
        }

        if (sim_time_ns <= anchor_sim_ns)
        {
            return anchor_wall_ns > 0
                       ? static_cast<uint64_t>(anchor_wall_ns)
                       : 0;
        }

        const long double elapsed_sim_ns =
            static_cast<long double>(sim_time_ns - anchor_sim_ns);
        const long double wall_offset_ns =
            elapsed_sim_ns / static_cast<long double>(speed);
        const long double deadline_ns =
            static_cast<long double>(anchor_wall_ns) + wall_offset_ns;

        if (deadline_ns <= 0.0L)
        {
            return 0;
        }

        constexpr long double max_deadline =
            static_cast<long double>(std::numeric_limits<uint64_t>::max());

        if (deadline_ns >= max_deadline)
        {
            return std::numeric_limits<uint64_t>::max();
        }

        return static_cast<uint64_t>(deadline_ns + 0.5L);
    }
}
