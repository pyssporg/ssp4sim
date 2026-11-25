#include "simulation/simulation_engine.hpp"

#include "ssp4cpp/ssp.hpp"
#include "ssp4cpp/utils/log.hpp"

#include "utils/time/timer.hpp"

#include "pre/pre_pipeline.hpp"
#include "pre/3_simulation_graph/elements/model_fmu.hpp"
#include "simulation/sim_setup.hpp"
#include "simulation/control/run_control.hpp"
#include "executor/executor_base.hpp"

#include "simulation/signal/sinks/csv_recorder_sink.hpp"
#include "simulation/signal/sinks/sqlite_recorder_sink.hpp"
#include "simulation/signal/recorder.hpp"

#include "utils/io/io.hpp"
#include "utils/primitives/map.hpp"
#include "utils/primitives/uuid.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssp4sim
{
    struct SimulationEngine::Impl
    {
        ssp4cpp::utils::log::Logger *log = nullptr;

        // Destruction is reversed: recorder and graph are released before
        // `conf` and `ssp`, which they were built from.
        std::unique_ptr<ssp4cpp::Ssp> ssp;
        std::unique_ptr<SharedConfig> conf;
        std::string session_uuid;

        pre::SimulationGraph sim_graph; // model graph
        pre::SimulationData sim;        // executors, access rules

        std::unique_ptr<signal::DataRecorder> recorder;

        std::shared_ptr<graph::Invocable> simulation_node;

        // One run control per engine. Executors share this instance so the
        // public control methods and the run thread observe the same state.
        std::shared_ptr<control::RunControl> control;

        bool initialized = false;
    };

    SimulationEngine::SimulationEngine(SharedConfig config,
                                       ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<Impl>())
    {
        p->log = log ? log : config.log;
        LOG_INFO(p->log, "[{func}] Setting up simulation engine", __func__);

        LOG_DEBUG(p->log, "[{func}] - Config loaded", __func__);

        p->conf = std::make_unique<SharedConfig>(std::move(config));

        LOG_INFO(p->log, "[{func}] - Working directory: {}", __func__, p->conf->working_dir.c_str());

        std::filesystem::create_directories(p->conf->working_dir);

        if (!p->conf->log_file.parent_path().empty())
        {
            std::filesystem::create_directories(p->conf->log_file.parent_path());
        }

        if (p->conf->level_terminal != "disable")
        {
            ssp4cpp::utils::log::add_console(quill::loglevel_from_string(p->conf->level_terminal));
        }

        if (p->conf->level_file != "disable")
        {
            ssp4cpp::utils::log::add_file_sink(p->conf->log_file.string(), quill::loglevel_from_string(p->conf->level_file));
        }

        if (p->conf->level_json != "disable")
        {
            ssp4cpp::utils::log::add_json_sink(p->conf->log_file.string() + ".json", quill::loglevel_from_string(p->conf->level_json));
        }

        if (p->conf->level_cutelog != "disable")
        {
            try
            {
                ssp4cpp::utils::log::add_cutelog_sink("127.0.0.1", 19996, quill::loglevel_from_string(p->conf->level_cutelog));
            }
            catch (const std::exception &e)
            {
                LOG_WARNING(p->log, "[{func}] Cutelog sink disabled: {}", __func__, e.what());
            }
        }

        ssp4cpp::utils::log::init_logging();

        p->log = ssp4cpp::utils::log::make_logger("ssp4sim.Simulator");

        LOG_DEBUG(p->log, "[{func}] - Importing SSP", __func__);
        p->ssp = std::make_unique<ssp4cpp::Ssp>(p->conf->ssp_path, p->conf->ssd);
        LOG_DEBUG(p->log, "[{func}] -- SSP: {ssp}", __func__, p->ssp->to_string());

        LOG_INFO(p->log, "[{func}] Creating simulation", __func__);

        p->session_uuid = utils::make_uuid_v4();

        if (p->conf->enable_recording)
        {
            p->recorder = std::make_unique<signal::DataRecorder>(p->conf->wait_for_recorder);

            if (p->conf->csv.enable)
            {
                p->recorder->add_sink(std::make_unique<signal::CsvRecorderSink>(p->conf->csv.file, p->conf->csv.interval, p->conf->csv.flush_interval, p->conf->record_derivatives));
            }

            if (p->conf->sqlite.enable)
            {
                p->recorder->add_sink(std::make_unique<signal::SqliteWALRecorderSink>(p->conf->working_dir, p->session_uuid, p->conf->sqlite.file, p->conf->record_derivatives));
            }
        }

        p->control = std::make_shared<control::RunControl>(
            p->conf->speed,
            p->conf->fmu.start_time);
    }

    SimulationEngine::~SimulationEngine() = default;

    bool SimulationEngine::initialized() const noexcept
    {
        return p->initialized;
    }

    void SimulationEngine::init()
    {
        if (p->initialized)
        {
            throw std::logic_error("Simulator::init() called twice");
        }

        LOG_INFO(p->log, "[{func}] Initializing simulation", __func__);

        p->sim_graph = pre::build_simulation_graph(p->ssp.get(), p->conf.get());

        // set up executors, data access rules, data recording mechanisms
        p->sim = pre::setup_sim_behaviour(p->sim_graph.models, p->recorder.get(), p->conf.get());
        p->simulation_node = p->sim.execution_node;

        LOG_INFO(p->log, "[{func}] - Init simulation graph", __func__);
        p->simulation_node->init();

        if (p->recorder)
        {
            LOG_INFO(p->log, "[{func}] - Initializing recorder", __func__);
            p->recorder->init();
        }

        p->control->mark_initialized();
        p->initialized = true;
    }

    RunResult SimulationEngine::run()
    {
        if (!p->initialized)
        {
            // Publish a terminal state before throwing so the start barrier
            // cannot hang on a pre-run failure.
            p->control->finish(RunResult::Failed);
            throw std::logic_error("SimulationEngine::run() called before init()");
        }

        auto sim_timer = utils::time::Timer();

        std::exception_ptr simulation_error;
        RunResult result = RunResult::Completed;
        bool begin_ok = false;
        uint64_t completed_ns = p->conf->fmu.start_time;

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

            if (p->conf->fmu.timestep == 0)
            {
                // A zero slice would never advance the outer loop.
                throw std::runtime_error(
                    "SimulationEngine::run(): simulation.timestep must be greater than zero");
            }

            if (p->recorder)
            {
                p->recorder->start_recording();
            }

            LOG_INFO(p->log, "[{func}] Starting simulation", __func__);

            const uint64_t end_time = p->conf->fmu.end_time;
            const uint64_t timestep = p->conf->fmu.timestep;
            uint64_t slice_start = p->conf->fmu.start_time;

            while (slice_start < end_time)
            {
                // The single control choke point: apply due actions and pace.
                if (!p->control->before_slice(slice_start))
                {
                    break;
                }

                const uint64_t slice_end = std::min(slice_start + timestep, end_time);
                p->simulation_node->invoke(
                    ssp4sim::graph::StepData(slice_start, slice_end));
                completed_ns = slice_end;
                slice_start = slice_end;
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
            p->control->publish_boundary(completed_ns);
        }

        auto sim_wall_time = sim_timer.stop();

        LOG_INFO(p->log, "[{func}] Total walltime: {walltime} ", __func__, utils::time::ns_to_s(sim_wall_time));

        // Flush in a guard so a recorder failure still publishes a terminal
        // state and is rethrown after finish().
        if (p->recorder)
        {
            try
            {
                p->recorder->stop_recording();
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
        }

        // Finalize after the flush: a stop request during the flush must still be
        // observed. finish() returns the effective result.
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

        uint64_t total_model_time = 0;
        for (auto &[key, node] : p->sim_graph.models)
        {
            auto model_walltime = node->walltime_ns;
            LOG_INFO(p->log, "[{func}] Model {model} walltime: {walltime}", __func__, node->name, utils::time::ns_to_s(model_walltime));
            total_model_time += model_walltime;
        }
        LOG_INFO(p->log, "[{func}] Model walltime: {walltime}", __func__, utils::time::ns_to_s(total_model_time));

        if (simulation_error)
        {
            std::rethrow_exception(simulation_error);
        }

        return result;
    }

    void SimulationEngine::pause(std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->pause(at_sim_time_ns);
    }

    void SimulationEngine::resume()
    {
        p->control->resume();
    }

    void SimulationEngine::stop(std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->stop(at_sim_time_ns);
    }

    void SimulationEngine::set_speed(double speed,
                                     std::optional<uint64_t> at_sim_time_ns)
    {
        p->control->set_speed(speed, at_sim_time_ns);
    }

    RunInfo SimulationEngine::info() const
    {
        return p->control->info();
    }

    RunState SimulationEngine::state() const
    {
        return p->control->state();
    }

    uint64_t SimulationEngine::sim_time_ns() const
    {
        return p->control->sim_time();
    }

    double SimulationEngine::speed() const
    {
        return p->control->speed();
    }

    void SimulationEngine::request_stop()
    {
        p->control->request_stop();
    }

    void SimulationEngine::wait_until_started()
    {
        p->control->wait_until_started();
    }
}
