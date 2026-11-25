#pragma once

#include "ssp4cpp/utils/log.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssp4sim
{
    /// Static parameters for the loop-aware (la2) executor family, parsed from
    /// the `simulation.executor.la2.*` key namespace. Consumed directly by
    /// `make_la2_stack` (executor/loop_aware/la2_builder.hpp).
    struct La2Options
    {
        std::string mode = "linear";          // "linear" | "factor" (aliases allowed)
        int iterations = -1;                  // < 0 -> SCC node count
        double factor = 0.8;
        uint64_t threshold = 0.0;               // nanoseconds; free-shrink cutoff for "factor" mode
        bool parallel = false;
    };

    /// Typed executor configuration. Parsed once from the JSON document by
    /// `ExecutorOptions::from_json` (called from `SharedConfig::load`);
    /// `ExecutorBuilder` and the executors it constructs consume these values
    /// and never read any global config.
    ///
    /// The legacy `simulation.executor.loop_aware.*` key namespace is
    /// intentionally not read here. `simulation.timestep` is not duplicated
    /// here: the macro step is owned by `FmuModelConfig` (single source of
    /// truth) and consumed by the engine-owned outer loop.
    struct ExecutorOptions
    {
        std::string method = "jacobi";

        // jacobi
        bool jacobi_parallel = false;
        int jacobi_method = 1; // 1 = TBB, 2 = spin pool, 3 = futures
        int workers = 5;

        // seidel
        bool seidel_parallel = false;

        // la2
        La2Options la2;

        /// Read the `simulation.executor.*` subtree from a parsed JSON
        /// document. The single parse site for all executor configuration.
        static ExecutorOptions from_json(const nlohmann::json &data);
    };

    /// Static model-level parameters for `graph::FmuModel`: the experiment
    /// span, tolerance and per-model toggles. Owns the experiment times
    /// (single source of truth for start/stop/timestep); nested in
    /// `SharedConfig` and handed to the model layer as a unit. Parsed
    /// centrally by `SharedConfig::load`.
    struct FmuModelConfig
    {
        bool forward_derivatives = true;
        bool fmu_logging = false;
        double tolerance = 0.0;
        uint64_t start_time = 0; // ns
        uint64_t timestep = 0;   // ns
        uint64_t end_time = 0;   // ns
    };

    struct SharedConfig
    {
        ssp4cpp::utils::log::Logger *log = nullptr;

    public:
        // Common
        std::string ssp_path;
        std::string ssd;

        std::filesystem::path working_dir = "./wd/default";

        // Executor selection and tuning, and model-level parameters (owns the
        // experiment times). Both are the single parse site for their slices.
        ExecutorOptions executor;
        FmuModelConfig fmu;

        // Wall-clock pace for the outer macro steps. 0 means run as fast as
        // possible; > 0 means simulation time advances `speed` times faster
        // than wall time. Parsed from `simulation.speed`.
        double speed = 0.0;

        // Recordings

        bool enable_recording = false;
        bool wait_for_recorder = false;
        bool record_inputs = false;
        bool record_derivatives = false;

        struct CsvRecordingConfig
        {
            bool enable = true;
            std::filesystem::path file;
            uint64_t interval = 0;         // ns, recording interval (row spacing)
            uint64_t flush_interval = 0;   // ns, wall-clock period to flush buffered rows
        };
        CsvRecordingConfig csv;
        struct SqliteRecordingConfig
        {
            bool enable = false;
            std::optional<std::filesystem::path> file;
        };
        SqliteRecordingConfig sqlite;

        // Logging

        std::filesystem::path log_file;
        std::filesystem::path start_value_log_file;

        std::string level_terminal;
        std::string level_file;
        std::string level_json;
        std::string level_cutelog;

        static SharedConfig from_file(const std::filesystem::path &path,
                                      ssp4cpp::utils::log::Logger *log = nullptr);
        static SharedConfig from_string(std::string_view config,
                                        ssp4cpp::utils::log::Logger *log = nullptr);
        static SharedConfig from_json(const nlohmann::json &data,
                                      ssp4cpp::utils::log::Logger *log = nullptr);

    private:
        SharedConfig() = default;
        void load(const nlohmann::json &data, ssp4cpp::utils::log::Logger *log);
    };

}
