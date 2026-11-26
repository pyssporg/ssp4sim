#include "simulator.hpp"

#include "simulation/runner/threaded_runner.hpp"

#include <memory>
#include <utility>

namespace ssp4sim
{
    struct SimulatorPrivate
    {
        std::unique_ptr<ThreadedRunner> runner;
    };

    Simulator::Simulator(const std::string config_path)
        : Simulator(SharedConfig::from_file(config_path))
    {
    }

    Simulator::Simulator(SharedConfig config,
                         ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<SimulatorPrivate>())
    {
        p->runner = std::make_unique<ThreadedRunner>(std::move(config), log);
    }

    Simulator::~Simulator() = default;

    void Simulator::init()
    {
        p->runner->init();
    }

    void Simulator::simulate()
    {
        start();
        wait();
    }

    void Simulator::start()
    {
        p->runner->start();
    }

    RunResult Simulator::wait()
    {
        return p->runner->wait();
    }

    void Simulator::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        p->runner->pause(at_sim_time_ns);
    }

    void Simulator::resume()
    {
        p->runner->resume();
    }

    void Simulator::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        p->runner->stop(at_sim_time_ns);
    }

    void Simulator::set_speed(double speed,
                              std::optional<uint64_t> at_sim_time_ns)
    {
        p->runner->set_speed(speed, at_sim_time_ns);
    }

    RunInfo Simulator::info() const
    {
        return p->runner->info();
    }

    void Simulator::wait_until_started()
    {
        p->runner->wait_until_started();
    }
}
