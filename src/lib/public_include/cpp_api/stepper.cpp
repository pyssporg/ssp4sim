#include "stepper.hpp"

#include "ssp4cpp/ssp.hpp"
#include "ssp4cpp/utils/log.hpp"

#include "pre/pre_pipeline.hpp"
#include "pre/3_simulation_graph/elements/model_fmu.hpp"
#include "simulation/sim_setup.hpp"
#include "executor/executor_base.hpp"

#include "simulation/signal/sinks/csv_recorder_sink.hpp"
#include "simulation/signal/sinks/sqlite_recorder_sink.hpp"
#include "simulation/signal/recorder.hpp"

#include "utils/io/io.hpp"
#include "utils/primitives/map.hpp"
#include "utils/primitives/uuid.hpp"
#include "utils/time/time.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssp4sim
{
    struct SimulationStepper::Impl
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

        bool initialized = false;
        bool started = false;
        bool finished = false;

        uint64_t sim_time_ns = 0;

        /// Advance one slice bounded by `limit_ns` and end_time. No pacing.
        StepStatus step_to(uint64_t limit_ns)
        {
            const uint64_t end_time = conf->fmu.end_time;

            if (sim_time_ns >= end_time)
            {
                return StepStatus::Completed;
            }

            if (limit_ns <= sim_time_ns)
            {
                return StepStatus::Advanced;
            }

            uint64_t slice_end = sim_time_ns + conf->fmu.timestep;
            slice_end = std::min(slice_end, limit_ns);
            slice_end = std::min(slice_end, end_time);

            simulation_node->invoke(graph::StepData(sim_time_ns, slice_end));
            sim_time_ns = slice_end;

            return sim_time_ns >= end_time ? StepStatus::Completed
                                           : StepStatus::Advanced;
        }
    };

    SimulationStepper::SimulationStepper(SharedConfig config,
                                         ssp4cpp::utils::log::Logger *log)
        : p(std::make_unique<Impl>())
    {
        p->log = log ? log : config.log;
        LOG_INFO(p->log, "[{func}] Setting up simulation stepper", __func__);

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

        p->sim_time_ns = p->conf->fmu.start_time;
    }

    SimulationStepper::~SimulationStepper() = default;

    bool SimulationStepper::initialized() const noexcept
    {
        return p->initialized;
    }

    void SimulationStepper::init()
    {
        if (p->initialized)
        {
            throw std::logic_error("SimulationStepper::init() called twice");
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

        p->initialized = true;
    }

    void SimulationStepper::start()
    {
        if (!p->initialized)
        {
            throw std::logic_error(
                "SimulationStepper::start() called before init()");
        }

        if (p->started || p->finished)
        {
            throw std::logic_error(
                "SimulationStepper::start() called more than once");
        }

        if (p->conf->fmu.timestep == 0)
        {
            throw std::runtime_error(
                "SimulationStepper::start(): simulation.timestep must be greater than zero");
        }

        if (p->recorder)
        {
            p->recorder->start_recording();
        }

        p->started = true;
    }

    StepStatus SimulationStepper::step()
    {
        if (!p->started || p->finished)
        {
            throw std::logic_error(
                "SimulationStepper::step() called before start() or after finish()");
        }

        if (p->sim_time_ns >= p->conf->fmu.end_time)
        {
            return StepStatus::Completed;
        }

        return p->step_to(p->sim_time_ns + p->conf->fmu.timestep);
    }

    StepStatus SimulationStepper::advance_to(uint64_t target_ns)
    {
        if (!p->started || p->finished)
        {
            throw std::logic_error(
                "SimulationStepper::advance_to() called before start() or after finish()");
        }

        const uint64_t end_time = p->conf->fmu.end_time;
        if (target_ns > end_time)
        {
            target_ns = end_time;
        }

        if (p->sim_time_ns >= end_time)
        {
            return StepStatus::Completed;
        }

        while (p->sim_time_ns < target_ns)
        {
            const StepStatus status = p->step_to(target_ns);
            if (status == StepStatus::Completed)
            {
                return status;
            }
        }

        return StepStatus::Advanced;
    }

    uint64_t SimulationStepper::sim_time_ns() const noexcept
    {
        return p->sim_time_ns;
    }

    uint64_t SimulationStepper::end_time_ns() const noexcept
    {
        return p->conf ? p->conf->fmu.end_time : 0;
    }

    void SimulationStepper::finish()
    {
        if (p->finished)
        {
            return;
        }

        p->finished = true;

        log_model_walltimes();

        if (p->recorder)
        {
            p->recorder->stop_recording();
        }
    }

    void SimulationStepper::log_model_walltimes() const
    {
        uint64_t total_model_time = 0;

        for (const auto &[key, node] : p->sim_graph.models)
        {
            const auto model_walltime = node->walltime_ns;
            LOG_INFO(p->log, "[{func}] Model {model} walltime: {walltime}",
                     __func__, node->name, utils::time::ns_to_s(model_walltime));
            total_model_time += model_walltime;
        }

        LOG_INFO(p->log, "[{func}] Model walltime: {walltime}", __func__,
                 utils::time::ns_to_s(total_model_time));
    }
}
