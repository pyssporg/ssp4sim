#include "simulator.hpp"

#include "simulation/simulation_engine.hpp"

#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ssp4sim
{
    struct SimulatorPrivate
    {
        std::unique_ptr<SimulationEngine> engine;

        // Run-thread ownership lives here; the execution body itself is
        // SimulationEngine::run().
        std::mutex run_mutex;
        std::thread run_thread;
        std::shared_future<RunResult> run_future;
        bool run_started = false;
    };

    Simulator::Simulator(const std::string config_path)
        : Simulator(SharedConfig::from_file(config_path))
    {
    }

    Simulator::Simulator(SharedConfig config,
                         ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<SimulatorPrivate>())
    {
        p->engine = std::make_unique<SimulationEngine>(std::move(config), log);
    }

    Simulator::~Simulator()
    {
        if (!p)
        {
            return;
        }

        // A live run must be stopped and joined before the engine (recorder,
        // graph and FMUs) is torn down. Stop exits at the next macro boundary
        // and the recorder still flushes partial results (see
        // SimulationEngine::run()).
        if (p->run_started)
        {
            p->engine->request_stop();
            try
            {
                wait();
            }
            catch (...)
            {
                // Destructors must not throw; run() already logged the failure.
            }
        }

        if (p->run_thread.joinable())
        {
            p->run_thread.join();
        }
    }

    void Simulator::init()
    {
        p->engine->init();
    }

    void Simulator::simulate()
    {
        start();
        wait();
    }

    void Simulator::start()
    {
        if (!p->engine->initialized())
        {
            throw std::runtime_error("Simulator::start() called before init()");
        }

        std::packaged_task<RunResult()> task(
            [engine = p->engine.get()]() { return engine->run(); });
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
        p->engine->wait_until_started();
    }

    RunResult Simulator::wait()
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

        // Rethrows the failure (if any) captured by SimulationEngine::run().
        return future.get();
    }

    void Simulator::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        p->engine->pause(at_sim_time_ns);
    }

    void Simulator::resume()
    {
        p->engine->resume();
    }

    void Simulator::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        p->engine->stop(at_sim_time_ns);
    }

    void Simulator::set_speed(double speed,
                              std::optional<uint64_t> at_sim_time_ns)
    {
        p->engine->set_speed(speed, at_sim_time_ns);
    }

    RunInfo Simulator::info() const
    {
        return p->engine->info();
    }

    void Simulator::wait_until_started()
    {
        p->engine->wait_until_started();
    }
}
