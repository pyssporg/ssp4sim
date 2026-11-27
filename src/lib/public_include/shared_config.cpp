#include "shared_config.hpp"

#include "utils/config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace ssp4sim
{
    ExecutorOptions ExecutorOptions::from_json(const nlohmann::json &data)
    {
        utils::Config cfg(data);
        ExecutorOptions o;
        o.method = cfg.getOr("simulation.executor.method", std::string("jacobi"));

        o.jacobi_parallel = cfg.getOr("simulation.executor.jacobi.parallel", false);
        o.jacobi_method = cfg.getOr("simulation.executor.jacobi.method", 1);
        o.workers = cfg.getOr("simulation.executor.thread_pool_workers", 5);

        o.seidel_parallel = cfg.getOr("simulation.executor.seidel.parallel", false);

        o.la2.mode = cfg.getOr("simulation.executor.la2.mode", std::string("linear"));
        o.la2.iterations = cfg.getOr("simulation.executor.la2.iterations", -1);
        o.la2.factor = cfg.getOr("simulation.executor.la2.factor", 0.8);
        o.la2.threshold = utils::time::s_to_ns(cfg.getOr("simulation.executor.la2.threshold", 0.0));
        o.la2.parallel = cfg.getOr("simulation.executor.la2.parallel", false);
        return o;
    }

    SharedConfig SharedConfig::from_json(const nlohmann::json &data,
                                         ssp4cpp::utils::log::Logger *log)
    {
        SharedConfig config;
        config.load(data, log);
        return config;
    }

    SharedConfig SharedConfig::from_string(std::string_view text,
                                           ssp4cpp::utils::log::Logger *log)
    {
        const utils::Config cfg = utils::Config::fromString(text);
        return from_json(*cfg.resolvePath(""), log);
    }

    SharedConfig SharedConfig::from_file(const std::filesystem::path &path,
                                         ssp4cpp::utils::log::Logger *log)
    {
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error("Config file does not exist: " + path.string());
        }

        const utils::Config cfg = utils::Config::fromFile(path);
        return from_json(*cfg.resolvePath(""), log);
    }

    void SharedConfig::load(const nlohmann::json &data,
                            ssp4cpp::utils::log::Logger *log)
    {
        this->log = log ? log : ssp4cpp::utils::log::simple_logger();

        utils::Config cfg(data);

        LOG_DEBUG(this->log, "[{}] Seting up common config", __func__);
        // Common
        ssp_path = cfg.getString("simulation.ssp");
        ssd = cfg.getOr("simulation.ssd", "SystemStructure.ssd");

        working_dir = std::filesystem::path(cfg.getOr("simulation.working_dir", "./wd/default"));

        // Optional checkpoint path policy. Absent (or null) leaves it
        // unset, which permits any path.
        if (const auto *snapshot_dir_node = cfg.resolvePath("simulation.snapshot.directory");
            snapshot_dir_node != nullptr && !snapshot_dir_node->is_null())
        {
            if (!snapshot_dir_node->is_string())
            {
                throw std::invalid_argument(
                    "simulation.snapshot.directory must be a string");
            }
            snapshot_dir =
                std::filesystem::path(snapshot_dir_node->get<std::string>());
        }

        // Model parameters (owns the experiment times).
        fmu.forward_derivatives = cfg.getOr("simulation.executor.forward_derivatives", true);
        fmu.fmu_logging = cfg.getOr("simulation.log.fmu", false);
        fmu.tolerance = cfg.getDouble("simulation.tolerance");
        fmu.start_time = utils::time::s_to_ns(cfg.getDouble("simulation.start_time"));
        fmu.timestep = utils::time::s_to_ns(cfg.getDouble("simulation.timestep"));
        fmu.end_time = utils::time::s_to_ns(cfg.getDouble("simulation.stop_time"));

        // Executor selection + tuning, parsed centrally here.
        executor = ExecutorOptions::from_json(data);

        // Wall-clock pace: 0 means run as fast as possible.
        speed = cfg.getOr("simulation.speed", 0.0);
        if (!std::isfinite(speed) || speed < 0.0)
        {
            throw std::runtime_error("simulation.speed must be finite and greater than or equal to zero");
        }

        // Recording

        csv.enable = cfg.getOr("simulation.recording.csv.enable", true);
        if (csv.enable)
        {
            const auto csv_interval_s = cfg.getOr("simulation.recording.csv.interval", 0.0);
            if (csv_interval_s < 0.0)
            {
                throw std::runtime_error("simulation.recording.csv.interval must be greater than or equal to zero");
            }
            csv.interval = utils::time::s_to_ns(csv_interval_s);

            const auto csv_flush_interval_s = cfg.getOr("simulation.recording.csv.flush_interval", 0.0);
            if (csv_flush_interval_s < 0.0)
            {
                throw std::runtime_error("simulation.recording.csv.flush_interval must be greater than or equal to zero");
            }
            csv.flush_interval = utils::time::s_to_ns(csv_flush_interval_s);

            auto default_result_file = working_dir / "result.csv";
            csv.file = std::filesystem::path(cfg.getOr("simulation.recording.csv.file", default_result_file.string()));
        }

        sqlite.enable = cfg.getOr("simulation.recording.sqlite.enable", false);
        if (sqlite.enable)
        {
            // When config key is absent or empty, file is nullopt (auto-generate).
            // When present with non-empty string, use it (backward compat, [TIME] supported).
            const auto file_str = cfg.getOr("simulation.recording.sqlite.file", "");
            if (!file_str.empty())
            {
                sqlite.file = std::filesystem::path(working_dir / file_str);
            }
        }
        wait_for_recorder = cfg.getOr("simulation.recording.wait_for", false);
        record_inputs = cfg.getOr("simulation.recording.record_inputs", false);
        record_derivatives = cfg.getOr("simulation.recording.record_derivatives", false);

        enable_recording = csv.enable || sqlite.enable;

        // Log
        auto default_log_file = working_dir / "sim.log";

        log_file = std::filesystem::path(cfg.getOr("simulation.log.file", default_log_file.string()));

        level_terminal = cfg.getOr("simulation.log.level_terminal", "debug");

        level_file = cfg.getOr("simulation.log.level_file", "disable");

        level_json = cfg.getOr("simulation.log.level_json", "disable");

        level_cutelog = cfg.getOr("simulation.log.level_cutelog", "disable");

        auto default_start_value_log_file = cfg.getOr("simulation.log.start_values", "start_values.csv");

        start_value_log_file = working_dir / default_start_value_log_file;

        LOG_DEBUG(this->log, "Setup of SharedConfig complete");
    }

}
