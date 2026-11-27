#include "simulator.hpp"

#include "simulation/runtime/simulation_runtime.hpp"

#include "utils/primitives/uuid.hpp"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssp4sim
{
    struct SimulatorPrivate
    {
        explicit SimulatorPrivate(SharedConfig stored)
            : config(std::move(stored))
        {
        }

        // Retained so a restore can be built from the configuration the
        // Simulator was created with.
        SharedConfig config;

        // Owns the core, run thread and outer loop behind the control channel.
        std::unique_ptr<runtime::SimulationRuntime> session;
    };

    Simulator::Simulator(const std::string config_path)
        : Simulator(SharedConfig::from_file(config_path))
    {
    }

    Simulator::Simulator(SharedConfig config,
                         ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<SimulatorPrivate>(config))
    {
        p->session = std::make_unique<runtime::SimulationRuntime>(
            std::move(config),
            log);
    }

    Simulator::~Simulator() = default;

    void Simulator::init()
    {
        p->session->init();
    }

    void Simulator::start()
    {
        p->session->start();
    }

    RunResult Simulator::wait()
    {
        return p->session->wait();
    }

    void Simulator::simulate()
    {
        p->session->simulate();
    }

    void Simulator::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        p->session->pause(at_sim_time_ns);
    }

    void Simulator::resume()
    {
        p->session->resume();
    }

    void Simulator::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        p->session->stop(at_sim_time_ns);
    }

    void Simulator::shutdown() noexcept
    {
        (void)p->session->shutdown();
    }

    void Simulator::set_speed(double speed,
                              std::optional<uint64_t> at_sim_time_ns)
    {
        p->session->set_speed(speed, at_sim_time_ns);
    }

    RunInfo Simulator::info() const
    {
        return p->session->info();
    }

    bool Simulator::snapshot(const std::filesystem::path &path)
    {
        return p->session->snapshot(path);
    }

    void Simulator::restore(const std::filesystem::path &path, bool start)
    {
        // Keep the retained config so repeated restores always derive from the
        // configuration this Simulator was created with.
        const SharedConfig original = p->config;

        auto result = Simulator::rebuild(this, p->config, &path);
        if (result.exception)
        {
            std::rethrow_exception(result.exception);
        }

        p->session = std::move(result.replacement->p->session);
        p->config = original;

        if (start)
        {
            Simulator::start();
        }
    }

    void Simulator::reset()
    {
        auto result = Simulator::rebuild(this, p->config, nullptr);
        if (result.exception)
        {
            std::rethrow_exception(result.exception);
        }

        p->session = std::move(result.replacement->p->session);
    }

    std::unique_ptr<Simulator> Simulator::create_initialized(SharedConfig config)
    {
        auto simulator = std::make_unique<Simulator>(std::move(config));
        simulator->init();
        return simulator;
    }

    std::unique_ptr<Simulator> Simulator::create_restored(
        SharedConfig config, const std::filesystem::path &path)
    {
        auto simulator = std::make_unique<Simulator>(std::move(config));
        simulator->init();
        simulator->p->session->load_snapshot(path);
        return simulator;
    }

    SharedConfig Simulator::prepare_restore_config(SharedConfig config,
                                                   double prior_speed)
    {
        config.speed = prior_speed;

        if (config.enable_recording && config.csv.enable &&
            !config.csv.file.empty())
        {
            // A CSV sink truncates on open; a new segment keeps the prior
            // run's output. The UUID avoids collisions between restores in the
            // same second.
            const auto &original = config.csv.file;
            const auto directory = original.parent_path();
            const auto stem = original.stem().string();
            const auto extension = original.extension().string();
            config.csv.file =
                directory /
                (stem + ".restored-" + utils::make_uuid_v4() + extension);
        }

        return config;
    }

    Simulator::RebuildResult Simulator::rebuild(
        Simulator *old,
        SharedConfig config,
        const std::filesystem::path *snapshot)
    {
        RebuildResult result;

        if (old)
        {
            result.old_info = old->info();
        }
        result.had_run =
            old && result.old_info.state != RunState::Created &&
            result.old_info.state != RunState::Initialized;
        const double prior_speed = old ? result.old_info.speed : 0.0;

        if (result.had_run)
        {
            result.old_run_error = old->p->session->shutdown();
            result.old_info = old->info();
        }

        try
        {
            if (snapshot)
            {
                config =
                    prepare_restore_config(std::move(config), prior_speed);
                result.replacement = create_restored(std::move(config), *snapshot);
            }
            else
            {
                result.replacement = create_initialized(std::move(config));
            }
        }
        catch (const std::exception &exception)
        {
            result.exception = std::current_exception();
            result.message = exception.what();
            result.replacement.reset();
        }
        catch (...)
        {
            result.exception = std::current_exception();
            result.message = "unknown error";
            result.replacement.reset();
        }

        return result;
    }
}
