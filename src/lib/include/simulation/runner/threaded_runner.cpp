#include "simulation/runner/threaded_runner.hpp"

#include "cpp_api/stepper.hpp"
#include "simulation/control/run_control.hpp"

#include "ssp4cpp/utils/log.hpp"

#include "utils/time/timer.hpp"

#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ssp4sim
{
    struct ThreadedRunner::Impl
    {
        ssp4cpp::utils::log::Logger *log = nullptr;

        std::unique_ptr<SimulationStepper> stepper;

        // One run control per runner. Public control methods and the run
        // thread observe the same instance.
        std::shared_ptr<control::RunControl> control;

        // Run-thread ownership lives here; the execution body itself is
        // ThreadedRunner::run().
        std::mutex run_mutex;
        std::thread run_thread;
        std::shared_future<RunResult> run_future;
        bool run_started = false;
    };

    ThreadedRunner::ThreadedRunner(SharedConfig config,
                                   ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<Impl>())
    {
        p->log = log ? log : config.log;

        // RunControl consumes the pace and start time; capture them before the
        // config is moved into the stepper.
        const double initial_speed = config.speed;
        const uint64_t start_time_ns = config.fmu.start_time;

        p->stepper = std::make_unique<SimulationStepper>(std::move(config), log);
        p->log = ssp4cpp::utils::log::make_logger("ssp4sim.Simulator");

        p->control = std::make_shared<control::RunControl>(
            initial_speed,
            start_time_ns);
    }

    ThreadedRunner::~ThreadedRunner()
    {
        if (!p)
        {
            return;
        }

        // A live run must be stopped and joined before the stepper (recorder,
        // graph and FMUs) is torn down. Stop exits at the next macro boundary
        // and the recorder still flushes partial results (see run()).
        if (p->run_started)
        {
            p->control->request_stop();
            try
            {
                wait();
            }
            catch (...)
            {
                // Destructors must not throw; run() already logged the failure.
            }
        }

        try
        {
            if (p->run_thread.joinable())
            {
                p->run_thread.join();
            }
        }
        catch (...)
        {
            // Destructors must not throw.
        }
    }

    bool ThreadedRunner::initialized() const noexcept
    {
        return p->stepper->initialized();
    }

    void ThreadedRunner::init()
    {
        p->stepper->init();
        p->control->mark_initialized();
    }

    void ThreadedRunner::start()
    {
        if (!p->stepper->initialized())
        {
            throw std::runtime_error("Simulator::start() called before init()");
        }

        std::packaged_task<RunResult()> task(
            [this]() { return run(); });
        auto future = task.get_future().share();

        {
            std::lock_guard<std::mutex> lock(p->run_mutex);
            if (p->run_started)
            {
                throw std::logic_error("Simulator::start() called more than once");
            }

            // Construct the thread before publishing run_started; if
            // construction throws, run_started stays false and wait() cannot
            // hang.
            std::thread thread(std::move(task));
            p->run_thread = std::move(thread);
            p->run_future = future;
            p->run_started = true;
        }

        // Start barrier: the run thread publishes Running or a terminal
        // pre-run state. Wait outside run_mutex so the run thread and wait()
        // are never blocked by the start call.
        p->control->wait_until_started();
    }

    RunResult ThreadedRunner::wait()
    {
        std::shared_future<RunResult> future;
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(p->run_mutex);
            if (!p->run_started)
            {
                throw std::logic_error("Simulator::wait() called before start()");
            }
            future = p->run_future;
            // Take ownership of the thread so the join happens outside the
            // mutex; a repeated wait() sees a non-joinable member and is a
            // no-op.
            thread = std::move(p->run_thread);
        }

        future.wait();
        if (thread.joinable())
        {
            thread.join();
        }

        // Rethrows the failure (if any) captured by run().
        return future.get();
    }

    RunResult ThreadedRunner::run()
    {
        if (!p->stepper->initialized())
        {
            // Publish a terminal state before throwing so the start barrier
            // cannot hang on a pre-run failure.
            p->control->finish(RunResult::Failed);
            throw std::logic_error("ThreadedRunner::run() called before init()");
        }

        auto sim_timer = utils::time::Timer();

        std::exception_ptr simulation_error;
        RunResult result = RunResult::Completed;
        bool begin_ok = false;

        try
        {
            begin_ok = p->control->begin_run();
            if (!begin_ok)
            {
                // A pre-run teardown already cancelled this run. Publish the
                // terminal state and leave without starting the recorder or
                // invoking the graph.
                return p->control->finish(RunResult::Stopped);
            }

            p->stepper->start();

            LOG_INFO(p->log, "[{func}] Starting simulation", __func__);

            for (;;)
            {
                // The single control choke point: apply due actions and pace.
                if (!p->control->before_slice(p->stepper->sim_time_ns()))
                {
                    break;
                }

                const StepStatus status = p->stepper->step();
                p->control->publish_boundary(p->stepper->sim_time_ns());

                if (status == StepStatus::Completed)
                {
                    break;
                }
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR(p->log, "Simulation failed! {error}", e.what());
            simulation_error = std::current_exception();
        }
        catch (...)
        {
            LOG_ERROR(p->log, "Simulation failed! {error}", "Unknown error");
            simulation_error = std::current_exception();
        }

        // Publish the last completed boundary before the recorder flush so a
        // completed run reports its end time, including time already advanced
        // while Stopping.
        if (begin_ok && !simulation_error)
        {
            p->control->publish_boundary(p->stepper->sim_time_ns());
        }

        auto sim_wall_time = sim_timer.stop();

        LOG_INFO(p->log, "[{func}] Total walltime: {walltime} ", __func__, utils::time::ns_to_s(sim_wall_time));

        // Flush in a guard so a recorder failure still publishes a terminal
        // state and is rethrown after finish().
        try
        {
            p->stepper->finish();
        }
        catch (const std::exception &e)
        {
            LOG_ERROR(p->log, "Recorder flush failed! {error}", e.what());
            if (!simulation_error)
            {
                simulation_error = std::current_exception();
            }
        }
        catch (...)
        {
            LOG_ERROR(p->log, "Recorder flush failed! {error}", "Unknown error");
            if (!simulation_error)
            {
                simulation_error = std::current_exception();
            }
        }

        // Finalize after the flush: a stop request during the flush must still
        // be observed. finish() returns the effective result.
        result = p->control->finish(simulation_error ? RunResult::Failed
                                                     : RunResult::Completed);

        if (simulation_error)
        {
            LOG_INFO(p->log, "[{func}] Simulation aborted\n", __func__);
        }
        else
        {
            LOG_INFO(p->log, "[{func}] Simulation completed\n", __func__);
        }

        if (simulation_error)
        {
            std::rethrow_exception(simulation_error);
        }

        return result;
    }

    void ThreadedRunner::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->pause(at_sim_time_ns);
    }

    void ThreadedRunner::resume()
    {
        p->control->resume();
    }

    void ThreadedRunner::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->stop(at_sim_time_ns);
    }

    void ThreadedRunner::set_speed(double speed,
                                   std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->set_speed(speed, at_sim_time_ns);
    }

    RunInfo ThreadedRunner::info() const
    {
        return p->control->info();
    }

    RunIdentity ThreadedRunner::run_identity() const
    {
        return p->stepper->run_identity();
    }

    void ThreadedRunner::request_stop()
    {
        p->control->request_stop();
    }

    void ThreadedRunner::wait_until_started()
    {
        p->control->wait_until_started();
    }
}
