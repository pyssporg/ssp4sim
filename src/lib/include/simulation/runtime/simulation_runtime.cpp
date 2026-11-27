#include "simulation/runtime/simulation_runtime.hpp"

#include "ssp4cpp/utils/log.hpp"

#include "utils/time/timer.hpp"

#include <cmath>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ssp4sim
{
    namespace runtime
    {
        namespace
        {
            std::exception_ptr control_error(const std::string &message)
            {
                return std::make_exception_ptr(std::logic_error(message));
            }
        }

        SimulationRuntime::SimulationRuntime(
            SharedConfig config,
            ssp4cpp::utils::log::Logger *log)
            : config_(config),
              clock_(config.speed, config.fmu.start_time)
        {
            log_ = log ? log : config.log;

            core_ = std::make_unique<SimulatorCore>(std::move(config), log);
            log_ = ssp4cpp::utils::log::make_logger("ssp4sim.Simulator");

            channel_ = std::make_unique<ControlChannel>(
                config_.speed,
                config_.fmu.start_time);
        }

        SimulationRuntime::~SimulationRuntime()
        {
            (void)shutdown();

            // A live run must be stopped and joined before the core (recorder,
            // graph and FMUs) is torn down.
            try
            {
                if (run_thread_.joinable())
                {
                    run_thread_.join();
                }
            }
            catch (...)
            {
                // Destructors must not throw.
            }
        }

        void SimulationRuntime::init()
        {
            core_->init();
            channel_->set_state(RunState::Initialized);
        }

        bool SimulationRuntime::has_run_thread()
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            return run_started_;
        }

        void SimulationRuntime::start()
        {
            if (!core_->initialized())
            {
                throw std::runtime_error(
                    "Simulator::start() called before init()");
            }

            std::packaged_task<RunResult()> task(
                [this]() { return run(); });
            auto future = task.get_future().share();

            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                if (run_started_)
                {
                    throw std::logic_error(
                        "Simulator::start() called more than once");
                }

                // Construct the thread before publishing run_started; if
                // construction throws, run_started stays false and wait()
                // cannot hang.
                std::thread thread(std::move(task));
                run_thread_ = std::move(thread);
                run_future_ = future;
                run_started_ = true;
            }

            Action action;
            action.kind = ActionKind::Start;
            const AppliedAction applied =
                channel_->post_and_wait(std::move(action));
            if (applied.status == ActionStatus::Nok)
            {
                if (applied.error)
                {
                    std::rethrow_exception(applied.error);
                }
                throw std::logic_error("Simulator::start() failed");
            }
        }

        RunResult SimulationRuntime::wait()
        {
            std::shared_future<RunResult> future;
            std::thread thread;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                if (!run_started_)
                {
                    throw std::logic_error(
                        "Simulator::wait() called before start()");
                }
                future = run_future_;
                // Take ownership of the thread so the join happens outside the
                // mutex; a repeated wait() sees a non-joinable member and is a
                // no-op.
                if (run_thread_.joinable())
                {
                    thread = std::move(run_thread_);
                }
            }

            (void)channel_->wait_terminal();
            if (thread.joinable())
            {
                thread.join();
            }

            // Rethrows the failure (if any) captured by run().
            const RunResult result = future.get();
            if (result == RunResult::Failed)
            {
                const ChannelStatus status = channel_->status();
                if (status.error)
                {
                    std::rethrow_exception(status.error);
                }
                throw std::runtime_error("Simulation failed");
            }
            return result;
        }

        void SimulationRuntime::simulate()
        {
            start();
            wait();
        }

        void SimulationRuntime::submit_and_wait(Action action)
        {
            const AppliedAction applied =
                channel_->post_and_wait(std::move(action));

            if (applied.status == ActionStatus::Nok)
            {
                if (applied.error)
                {
                    std::rethrow_exception(applied.error);
                }
                throw std::logic_error("Simulator control action was rejected");
            }
        }

        void SimulationRuntime::pause(std::optional<uint64_t> at_sim_time_ns)
        {
            if (at_sim_time_ns)
            {
                Action action;
                action.kind = ActionKind::Pause;
                action.at_ns = *at_sim_time_ns;
                (void)channel_->post(std::move(action));
                return;
            }

            if (channel_->status().state != RunState::Running)
            {
                throw std::logic_error(
                    "Simulator::pause() requires a Running simulation");
            }

            Action action;
            action.kind = ActionKind::Pause;
            submit_and_wait(std::move(action));
        }

        void SimulationRuntime::resume()
        {
            if (channel_->status().state != RunState::Paused)
            {
                throw std::logic_error(
                    "Simulator::resume() requires a Paused simulation");
            }

            Action action;
            action.kind = ActionKind::Resume;
            submit_and_wait(std::move(action));
        }

        void SimulationRuntime::stop(std::optional<uint64_t> at_sim_time_ns)
        {
            const RunState state = channel_->status().state;

            if (state == RunState::Created || state == RunState::Initialized)
            {
                throw std::logic_error(
                    "Simulator::stop() requires a Running or Paused simulation");
            }

            if (state == RunState::Stopping || is_terminal(state))
            {
                return;
            }

            Action action;
            action.kind = ActionKind::Stop;
            if (at_sim_time_ns)
            {
                action.at_ns = *at_sim_time_ns;
                (void)channel_->post(std::move(action));
                return;
            }

            const AppliedAction applied =
                channel_->post_and_wait(std::move(action));
            if (applied.status == ActionStatus::Nok && !channel_->is_closed())
            {
                if (applied.error)
                {
                    std::rethrow_exception(applied.error);
                }
                throw std::logic_error("Simulator::stop() failed");
            }
        }

        void SimulationRuntime::set_speed(
            double speed,
            std::optional<uint64_t> at_sim_time_ns)
        {
            if (!std::isfinite(speed) || speed < 0.0)
            {
                throw std::invalid_argument(
                    "Simulator::set_speed() requires a finite speed >= 0");
            }

            if (at_sim_time_ns)
            {
                Action action;
                action.kind = ActionKind::SetSpeed;
                action.at_ns = *at_sim_time_ns;
                action.speed = speed;
                (void)channel_->post(std::move(action));
                return;
            }

            const RunState state = channel_->status().state;
            if (state != RunState::Initialized &&
                state != RunState::Running &&
                state != RunState::Paused)
            {
                throw std::logic_error(
                    "Simulator::set_speed() requires an Initialized, Running "
                    "or Paused simulation");
            }

            if (state == RunState::Initialized && !has_run_thread())
            {
                // No consumer exists before start(); the action is a published
                // speed update and must not wait on the run thread.
                clock_.apply_speed(speed, core_->sim_time_ns());
                channel_->set_speed(speed);
                return;
            }

            Action action;
            action.kind = ActionKind::SetSpeed;
            action.speed = speed;
            submit_and_wait(std::move(action));
        }

        RunInfo SimulationRuntime::info() const
        {
            const ChannelStatus status = channel_->status();
            return RunInfo{status.state, status.sim_time_ns, status.speed};
        }

        bool SimulationRuntime::snapshot(const std::filesystem::path &path)
        {
            Action action;
            action.kind = ActionKind::Snapshot;
            action.path = path.string();
            const AppliedAction applied =
                channel_->post_and_wait(std::move(action));
            return applied.status == ActionStatus::Ok;
        }

        std::string SimulationRuntime::shutdown() noexcept
        {
            try
            {
                if (!has_run_thread())
                {
                    // No consumer exists, so publish the pre-run cancellation
                    // directly.
                    if (!is_terminal(channel_->status().state))
                    {
                        channel_->set_state(RunState::Stopped);
                    }
                    return {};
                }

                if (channel_->is_closed())
                {
                    return {};
                }

                Action action;
                action.kind = ActionKind::Stop;
                (void)channel_->post(std::move(action));

                try
                {
                    (void)wait();
                }
                catch (const std::exception &error)
                {
                    return error.what();
                }
                catch (...)
                {
                    return "unknown error";
                }

                return {};
            }
            catch (...)
            {
                return "unknown error";
            }
        }

        void SimulationRuntime::load_snapshot(
            const std::filesystem::path &path)
        {
            core_->load_snapshot(path);
            // Keep the reported time coherent with the restored core before
            // the run starts.
            channel_->publish_time(core_->sim_time_ns());
        }

        std::optional<uint64_t> SimulationRuntime::next_pacing_deadline() const
        {
            const double speed = channel_->status().speed;
            if (!(speed > 0.0) || !std::isfinite(speed))
            {
                return std::nullopt;
            }

            const uint64_t deadline = PacingClock::deadline_ns(
                clock_.anchor_wall_ns(),
                clock_.anchor_sim_ns(),
                core_->sim_time_ns(),
                speed);

            if (deadline == 0)
            {
                return std::nullopt;
            }

            return deadline;
        }

        bool SimulationRuntime::apply(Action &action)
        {
            const ChannelStatus status = channel_->status();

            switch (action.kind)
            {
            case ActionKind::Start:
                if (status.state == RunState::Initialized)
                {
                    try
                    {
                        core_->start();
                    }
                    catch (...)
                    {
                        const std::exception_ptr error =
                            std::current_exception();
                        channel_->publish_error(error);
                        channel_->set_state(RunState::Failed);
                        channel_->settle(action.seq, ActionStatus::Nok, error);
                        return false;
                    }

                    clock_.anchor_at(core_->sim_time_ns());
                    channel_->set_state(RunState::Running);
                    channel_->settle(action.seq, ActionStatus::Ok);
                    return true;
                }

                if (status.state == RunState::Stopping ||
                    is_terminal(status.state))
                {
                    channel_->settle(action.seq, ActionStatus::Ok);
                    return true;
                }

                {
                    const std::exception_ptr error = control_error(
                        "Simulator::start() called in an invalid state");
                    channel_->publish_error(error);
                    channel_->set_state(RunState::Failed);
                    channel_->settle(action.seq, ActionStatus::Nok, error);
                }
                return false;

            case ActionKind::Pause:
                if (status.state == RunState::Running)
                {
                    channel_->set_state(RunState::Paused);
                    channel_->settle(action.seq, ActionStatus::Ok);
                }
                else
                {
                    channel_->settle(
                        action.seq, ActionStatus::Nok,
                        control_error(
                            "Simulator::pause() requires a Running simulation"));
                }
                return true;

            case ActionKind::Resume:
                if (status.state == RunState::Paused)
                {
                    clock_.anchor_at(core_->sim_time_ns());
                    channel_->set_state(RunState::Running);
                    channel_->settle(action.seq, ActionStatus::Ok);
                }
                else
                {
                    channel_->settle(
                        action.seq, ActionStatus::Nok,
                        control_error(
                            "Simulator::resume() requires a Paused simulation"));
                }
                return true;

            case ActionKind::Stop:
                switch (status.state)
                {
                case RunState::Running:
                case RunState::Paused:
                    channel_->set_state(RunState::Stopping);
                    channel_->settle(action.seq, ActionStatus::Ok);
                    break;
                case RunState::Initialized:
                    channel_->set_state(RunState::Stopped);
                    channel_->settle(action.seq, ActionStatus::Ok);
                    break;
                case RunState::Stopping:
                case RunState::Completed:
                case RunState::Stopped:
                case RunState::Failed:
                    channel_->settle(action.seq, ActionStatus::Ok);
                    break;
                default:
                    channel_->settle(
                        action.seq, ActionStatus::Nok,
                        control_error(
                            "Simulator::stop() requires a Running or Paused "
                            "simulation"));
                    break;
                }
                return true;

            case ActionKind::SetSpeed:
                if (std::isfinite(action.speed) && action.speed >= 0.0)
                {
                    clock_.apply_speed(action.speed, core_->sim_time_ns());
                    channel_->set_speed(action.speed);
                    channel_->settle(action.seq, ActionStatus::Ok);
                }
                else
                {
                    channel_->settle(
                        action.seq, ActionStatus::Nok,
                        std::make_exception_ptr(std::invalid_argument(
                            "Simulator::set_speed() requires a finite speed "
                            ">= 0")));
                }
                return true;

            case ActionKind::Snapshot:
                if (status.state != RunState::Running &&
                    status.state != RunState::Paused)
                {
                    channel_->settle(
                        action.seq, ActionStatus::Nok,
                        control_error(
                            "Simulator::snapshot() requires a Running or "
                            "Paused simulation"));
                    return true;
                }

                try
                {
                    if (core_->save_snapshot(action.path))
                    {
                        channel_->settle(action.seq, ActionStatus::Ok);
                    }
                    else
                    {
                        channel_->settle(
                            action.seq, ActionStatus::Nok,
                            std::make_exception_ptr(std::runtime_error(
                                "snapshot save failed")));
                    }
                }
                catch (...)
                {
                    channel_->settle(action.seq, ActionStatus::Nok,
                                    std::current_exception());
                }
                return true;
            }

            channel_->settle(
                action.seq, ActionStatus::Nok,
                control_error("unknown control action"));
            return true;
        }

        RunResult SimulationRuntime::run()
        {
            auto sim_timer = utils::time::Timer();

            std::exception_ptr simulation_error;
            bool completed = false;

            try
            {
                LOG_INFO(log_, "[{func}] Starting simulation", __func__);

                for (;;)
                {
                    if (auto action =
                            channel_->take_due(core_->sim_time_ns()))
                    {
                        if (!apply(*action))
                        {
                            break;
                        }
                        continue;
                    }

                    ChannelStatus status = channel_->status();

                    if (status.state == RunState::Running)
                    {
                        if (const auto deadline = next_pacing_deadline())
                        {
                            channel_->wait_for_work(
                                core_->sim_time_ns(), deadline);

                            // A due action or a state change interrupts the
                            // pacing wait before this step.
                            if (auto action =
                                    channel_->take_due(core_->sim_time_ns()))
                            {
                                if (!apply(*action))
                                {
                                    break;
                                }
                                continue;
                            }

                            status = channel_->status();
                            if (status.state != RunState::Running)
                            {
                                continue;
                            }
                        }

                        const StepStatus step = core_->step();
                        channel_->publish_time(core_->sim_time_ns());

                        if (step == StepStatus::Completed)
                        {
                            completed = true;

                            // Actions due exactly at the final time still get
                            // applied, so a stop scheduled at end_time yields
                            // Stopped rather than Completed.
                            while (auto action =
                                       channel_->take_due(core_->sim_time_ns()))
                            {
                                if (!apply(*action))
                                {
                                    break;
                                }
                            }
                            break;
                        }
                        continue;
                    }

                    if (status.state == RunState::Stopping ||
                        is_terminal(status.state))
                    {
                        break;
                    }

                    // Paused (or an unexpected pre-run state) parks until an
                    // action or a state change.
                    channel_->wait_for_work(core_->sim_time_ns(),
                                            std::nullopt);
                }
            }
            catch (const std::exception &error)
            {
                LOG_ERROR(log_, "Simulation failed! {error}", error.what());
                simulation_error = std::current_exception();
            }
            catch (...)
            {
                LOG_ERROR(log_, "Simulation failed! {error}", "Unknown error");
                simulation_error = std::current_exception();
            }

            // Close first so blocked control callers never hang.
            channel_->close(simulation_error);

            // Publish the final simulation time before the recorder flush so a
            // completed run reports its end time, including time already
            // advanced while Stopping.
            channel_->publish_time(core_->sim_time_ns());

            try
            {
                core_->finish();
            }
            catch (const std::exception &error)
            {
                LOG_ERROR(log_, "Recorder flush failed! {error}", error.what());
                if (!simulation_error)
                {
                    simulation_error = std::current_exception();
                }
            }
            catch (...)
            {
                LOG_ERROR(log_, "Recorder flush failed! {error}", "Unknown error");
                if (!simulation_error)
                {
                    simulation_error = std::current_exception();
                }
            }

            const ChannelStatus final_status = channel_->status();
            RunState terminal = final_status.state;

            if (!is_terminal(terminal))
            {
                if (simulation_error)
                {
                    terminal = RunState::Failed;
                }
                else if (terminal == RunState::Stopping)
                {
                    terminal = RunState::Stopped;
                }
                else if (completed)
                {
                    terminal = RunState::Completed;
                }
                else
                {
                    terminal = RunState::Stopped;
                }
            }

            if (!simulation_error && terminal == RunState::Failed)
            {
                simulation_error =
                    final_status.error
                        ? final_status.error
                        : std::make_exception_ptr(
                              std::runtime_error("Simulation failed"));
            }

            if (simulation_error)
            {
                channel_->publish_error(simulation_error);
            }
            channel_->set_state(terminal);

            const auto wall_time = sim_timer.stop();
            LOG_INFO(log_, "[{func}] Total walltime: {walltime} ", __func__,
                     utils::time::ns_to_s(wall_time));

            if (simulation_error)
            {
                LOG_INFO(log_, "[{func}] Simulation aborted\n", __func__);
                std::rethrow_exception(simulation_error);
            }

            LOG_INFO(log_, "[{func}] Simulation completed\n", __func__);
            return terminal_result(terminal);
        }
    }
}
