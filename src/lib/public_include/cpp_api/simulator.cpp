#include "simulator.hpp"

#include "simulation/runtime/simulator_rebuild.hpp"
#include "simulation/runtime/simulation_runtime.hpp"

#include "utils/primitives/uuid.hpp"

#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssp4sim
{
    using runtime::Action;
    using runtime::ActionKind;
    using runtime::ActionStatus;
    using runtime::AppliedAction;
    using runtime::ChannelStatus;

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

    Simulator::~Simulator()
    {
        shutdown();
    }

    void Simulator::init()
    {
        p->session->init();
    }

    void Simulator::start()
    {
        p->session->start_thread();

        Action action;
        action.kind = ActionKind::Start;
        const AppliedAction applied =
            p->session->control().post_and_wait(std::move(action));
        if (applied.status == ActionStatus::Nok)
        {
            if (applied.error)
            {
                std::rethrow_exception(applied.error);
            }
            throw std::logic_error("Simulator::start() failed");
        }
    }

    RunResult Simulator::wait()
    {
        return p->session->wait();
    }

    void Simulator::simulate()
    {
        start();
        wait();
    }

    void Simulator::submit_and_wait(runtime::Action action)
    {
        const AppliedAction applied =
            p->session->control().post_and_wait(std::move(action));

        if (applied.status == ActionStatus::Nok)
        {
            if (applied.error)
            {
                std::rethrow_exception(applied.error);
            }
            throw std::logic_error("Simulator control action was rejected");
        }
    }

    void Simulator::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        if (at_sim_time_ns)
        {
            Action action;
            action.kind = ActionKind::Pause;
            action.at_ns = *at_sim_time_ns;
            (void)p->session->control().post(std::move(action));
            return;
        }

        if (p->session->control().status().state != RunState::Running)
        {
            throw std::logic_error(
                "Simulator::pause() requires a Running simulation");
        }

        Action action;
        action.kind = ActionKind::Pause;
        submit_and_wait(std::move(action));
    }

    void Simulator::resume()
    {
        if (p->session->control().status().state != RunState::Paused)
        {
            throw std::logic_error(
                "Simulator::resume() requires a Paused simulation");
        }

        Action action;
        action.kind = ActionKind::Resume;
        submit_and_wait(std::move(action));
    }

    void Simulator::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        const RunState state = p->session->control().status().state;

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
            (void)p->session->control().post(std::move(action));
            return;
        }

        const AppliedAction applied =
            p->session->control().post_and_wait(std::move(action));
        if (applied.status == ActionStatus::Nok &&
            !p->session->control().is_closed())
        {
            if (applied.error)
            {
                std::rethrow_exception(applied.error);
            }
            throw std::logic_error("Simulator::stop() failed");
        }
    }

    std::string Simulator::shutdown_report() noexcept
    {
        try
        {
            if (!p || !p->session)
            {
                return {};
            }

            if (!p->session->started())
            {
                p->session->cancel_before_start();
                return {};
            }

            Action action;
            action.kind = ActionKind::Stop;
            (void)p->session->control().post(std::move(action));
            return p->session->join();
        }
        catch (...)
        {
            return "unknown error";
        }
    }

    void Simulator::shutdown() noexcept
    {
        (void)shutdown_report();
    }

    void Simulator::set_speed(double speed,
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
            (void)p->session->control().post(std::move(action));
            return;
        }

        const RunState state = p->session->control().status().state;
        if (state != RunState::Initialized && state != RunState::Running &&
            state != RunState::Paused)
        {
            throw std::logic_error(
                "Simulator::set_speed() requires an Initialized, Running "
                "or Paused simulation");
        }

        if (state == RunState::Initialized && !p->session->started())
        {
            // No consumer exists before start(); the action is a published
            // speed update and must not wait on the run thread.
            p->session->set_speed_before_start(speed);
            return;
        }

        Action action;
        action.kind = ActionKind::SetSpeed;
        action.speed = speed;
        submit_and_wait(std::move(action));
    }

    RunInfo Simulator::info() const
    {
        const ChannelStatus status = p->session->control().status();
        return RunInfo{status.state, status.sim_time_ns, status.speed};
    }

    bool Simulator::snapshot(const std::filesystem::path &path)
    {
        // No consumer exists before the run starts, and a terminal run cannot
        // apply the action; reject without posting.
        const RunState state = p->session->control().status().state;
        if (state != RunState::Running && state != RunState::Paused)
        {
            return false;
        }

        Action action;
        action.kind = ActionKind::Snapshot;
        action.path = path.string();
        const AppliedAction applied =
            p->session->control().post_and_wait(std::move(action));
        return applied.status == ActionStatus::Ok;
    }

    void Simulator::restore(const std::filesystem::path &path, bool start)
    {
        // Keep the retained config so repeated restores always derive from the
        // configuration this Simulator was created with.
        const SharedConfig original = p->config;

        auto result = detail::RebuildAccess::rebuild(this, p->config, &path);
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
        auto result = detail::RebuildAccess::rebuild(this, p->config, nullptr);
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
}
