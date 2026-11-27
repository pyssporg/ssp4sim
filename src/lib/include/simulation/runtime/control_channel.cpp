#include "simulation/runtime/control_channel.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace ssp4sim::runtime
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

    ControlChannel::ControlChannel(double initial_speed, uint64_t start_time_ns)
        : sim_time_ns_(start_time_ns),
          speed_(initial_speed)
    {
    }

    uint64_t ControlChannel::post(Action action)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        action.seq = next_seq_++;

        if (!closed_)
        {
            queue_.emplace(action.at_ns, std::move(action));
        }

        cv_.notify_all();
        return action.seq;
    }

    AppliedAction ControlChannel::post_and_wait(Action action)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        action.seq = next_seq_++;
        const uint64_t seq = action.seq;

        if (closed_)
        {
            std::exception_ptr error = error_;
            if (!error)
            {
                error = std::make_exception_ptr(
                    std::logic_error("run ended before action"));
            }
            return AppliedAction{ActionStatus::Nok, error};
        }

        // Register before enqueueing: the runtime cannot take the action until
        // this thread releases the lock, so the completion is never missed.
        awaited_.insert(seq);
        queue_.emplace(action.at_ns, std::move(action));
        cv_.notify_all();

        cv_.wait(lock, [this, seq]() { return applied_seq_ >= seq; });

        const auto found = done_.find(seq);
        if (found == done_.end())
        {
            return AppliedAction{ActionStatus::Nok, error_};
        }

        const DoneAction &record = found->second;
        const AppliedAction applied{record.status, record.error};
        done_.erase(found);
        return applied;
    }

    ChannelStatus ControlChannel::status() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ChannelStatus{state_, sim_time_ns_, speed_, applied_seq_, error_};
    }

    std::size_t ControlChannel::pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::optional<Action> ControlChannel::take_due(uint64_t sim_time_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty())
        {
            return std::nullopt;
        }

        auto it = queue_.begin();
        const uint64_t due = it->first;
        if (due != 0 && due > sim_time_ns)
        {
            return std::nullopt;
        }

        Action action = std::move(it->second);
        queue_.erase(it);
        return action;
    }

    bool ControlChannel::has_due_locked(uint64_t sim_time_ns) const
    {
        if (queue_.empty())
        {
            return false;
        }

        const uint64_t due = queue_.begin()->first;
        return due == 0 || due <= sim_time_ns;
    }

    void ControlChannel::wait_for_work(
        uint64_t sim_time_ns,
        std::optional<uint64_t> wall_deadline_ns)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        for (;;)
        {
            if (closed_ || has_due_locked(sim_time_ns))
            {
                return;
            }

            // The loop breaks on Stopping/terminal before parking; returning
            // here also covers a state published by a concurrent teardown.
            if (is_terminal(state_) || state_ == RunState::Stopping)
            {
                return;
            }

            if (!wall_deadline_ns)
            {
                cv_.wait(lock);
                continue;
            }

            const auto deadline = std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(
                    static_cast<int64_t>(*wall_deadline_ns)));

            if (std::chrono::steady_clock::now() >= deadline)
            {
                return;
            }

            cv_.wait_until(lock, deadline);
        }
    }

    void ControlChannel::publish_time(uint64_t sim_time_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (sim_time_ns > sim_time_ns_)
        {
            sim_time_ns_ = sim_time_ns;
        }
        cv_.notify_all();
    }

    void ControlChannel::set_speed(double speed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        speed_ = speed;
        cv_.notify_all();
    }

    void ControlChannel::set_state(RunState state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        cv_.notify_all();
    }

    void ControlChannel::publish_error(std::exception_ptr error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(error);
        cv_.notify_all();
    }

    void ControlChannel::settle(uint64_t seq, ActionStatus status,
                                std::exception_ptr error)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (error)
        {
            error_ = error;
        }

        if (awaited_.erase(seq) > 0)
        {
            done_[seq] = DoneAction{status, std::move(error)};
        }

        applied_seq_ = std::max(applied_seq_, seq);
        cv_.notify_all();
    }

    void ControlChannel::close(std::exception_ptr error)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_)
        {
            if (error)
            {
                error_ = std::move(error);
            }
            cv_.notify_all();
            return;
        }

        closed_ = true;

        if (error)
        {
            error_ = error;
        }

        const std::exception_ptr settled_error =
            error ? error
                  : std::make_exception_ptr(
                        std::logic_error("run ended before action"));

        for (auto &entry : queue_)
        {
            Action &action = entry.second;
            action.status = ActionStatus::Nok;
            if (awaited_.erase(action.seq) > 0)
            {
                done_[action.seq] = DoneAction{ActionStatus::Nok, settled_error};
            }
            applied_seq_ = std::max(applied_seq_, action.seq);
        }
        queue_.clear();
        awaited_.clear();

        cv_.notify_all();
    }

    RunState ControlChannel::wait_terminal()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return is_terminal(state_); });
        return state_;
    }

    uint64_t ControlChannel::applied_seq() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return applied_seq_;
    }

    bool ControlChannel::is_closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
}
