#include <catch2/catch_test_macros.hpp>

#include "c_api/simulator_core_c_api.h"

#include "reference_ssp_fixture.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    constexpr uint64_t kTimestepNs = 50000000ULL;   // 0.05 s
    constexpr uint64_t kEndTimeNs = 300000000ULL;   // 0.30 s

    fs::path write_core_config(const fs::path &workdir,
                               double stop_time = 0.3,
                               double timestep = 0.05)
    {
        const auto ssp_root =
            ssp4sim::tests::reference_ssp::fixture_root("signal_step_gain");
        REQUIRE(fs::exists(ssp_root / "SystemStructure.ssd"));

        fs::create_directories(workdir);

        nlohmann::json config;
        config["simulation"] = {
            {"ssp", ssp_root.string()},
            {"start_time", 0.0},
            {"stop_time", stop_time},
            {"timestep", timestep},
            {"tolerance", 1e-6},
            {"speed", 0.0},
            {"working_dir", workdir.string()},
            {"executor", {{"method", "jacobi"}, {"forward_derivatives", false}}},
            {"log", {{"fmu", false},
                     {"level_terminal", "disable"},
                     {"level_file", "disable"},
                     {"level_json", "disable"},
                     {"level_cutelog", "disable"}}},
            {"recording", {{"csv", {{"enable", false}}},
                           {"sqlite", {{"enable", false}}}}}};

        const auto config_path = workdir / "config.json";
        std::ofstream output(config_path, std::ios::trunc);
        REQUIRE(output.is_open());
        output << config.dump(2);
        return config_path;
    }

    void require_last_error_set()
    {
        REQUIRE(ssp4sim_last_error() != nullptr);
        REQUIRE(ssp4sim_last_error()[0] != '\0');
    }
}

// ---------------------------------------------------------------------------
// Description: Exercises the synchronous SimulatorCore C ABI end to end.
// Rationale:   The SimulatorCore is the synchronous core behind the threaded C ABI;
//              its C surface must advance to exact times and report completion
//              without threading, pacing, or lifecycle state.
// ---------------------------------------------------------------------------
TEST_CASE("SimulatorCore C ABI happy path", "[high_level][c_api][simulator_core]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "simulator_core_c_api_happy";
    const auto config_path = write_core_config(workdir);

    ssp4sim_simulator_core_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_core_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(handle != nullptr);

    REQUIRE(ssp4sim_simulator_core_init(handle) == SSP4SIM_STATUS_OK);

    ssp4sim_simulator_core_info info{};
    REQUIRE(ssp4sim_simulator_core_get_info(handle, &info) == SSP4SIM_STATUS_OK);
    REQUIRE(info.sim_time_ns == 0);
    REQUIRE(info.end_time_ns > 0);
    const uint64_t end_time_ns = info.end_time_ns;

    REQUIRE(ssp4sim_simulator_core_start(handle) == SSP4SIM_STATUS_OK);

    ssp4sim_step_status status = SSP4SIM_STEP_COMPLETED;
    REQUIRE(ssp4sim_simulator_core_step(handle, &status) == SSP4SIM_STATUS_OK);
    REQUIRE(status == SSP4SIM_STEP_ADVANCED);
    REQUIRE(ssp4sim_simulator_core_get_info(handle, &info) == SSP4SIM_STATUS_OK);
    REQUIRE(info.sim_time_ns == kTimestepNs);

    // A non-aligned target is consumed exactly, not rounded to a macro step.
    const uint64_t non_aligned_ns = 123456789ULL;
    REQUIRE(non_aligned_ns % kTimestepNs != 0);
    REQUIRE(ssp4sim_simulator_core_advance_to(handle, non_aligned_ns, &status) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(status == SSP4SIM_STEP_ADVANCED);
    REQUIRE(ssp4sim_simulator_core_get_info(handle, &info) == SSP4SIM_STATUS_OK);
    REQUIRE(info.sim_time_ns == non_aligned_ns);

    // A target beyond the end is clamped to the configured end time.
    REQUIRE(ssp4sim_simulator_core_advance_to(handle, end_time_ns * 2, &status) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(status == SSP4SIM_STEP_COMPLETED);
    REQUIRE(ssp4sim_simulator_core_get_info(handle, &info) == SSP4SIM_STATUS_OK);
    REQUIRE(info.sim_time_ns == end_time_ns);

    // Extra steps stay completed.
    REQUIRE(ssp4sim_simulator_core_step(handle, &status) == SSP4SIM_STATUS_OK);
    REQUIRE(status == SSP4SIM_STEP_COMPLETED);

    REQUIRE(ssp4sim_simulator_core_finish(handle) == SSP4SIM_STATUS_OK);
    ssp4sim_simulator_core_destroy(handle);
}

TEST_CASE("SimulatorCore C ABI rejects null arguments", "[high_level][c_api][simulator_core]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "simulator_core_c_api_args";
    const auto config_path = write_core_config(workdir);

    ssp4sim_simulator_core_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_core_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(handle != nullptr);

    ssp4sim_simulator_core_handle *null_config_handle =
        reinterpret_cast<ssp4sim_simulator_core_handle *>(0x1);
    REQUIRE(ssp4sim_simulator_core_create(nullptr, &null_config_handle) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(null_config_handle == nullptr);
    require_last_error_set();

    REQUIRE(ssp4sim_simulator_core_create(config_path.string().c_str(), nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();

    ssp4sim_step_status status = SSP4SIM_STEP_ADVANCED;
    REQUIRE(ssp4sim_simulator_core_step(handle, nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();

    REQUIRE(ssp4sim_simulator_core_advance_to(handle, 0, nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();

    ssp4sim_simulator_core_info info{};
    REQUIRE(ssp4sim_simulator_core_get_info(handle, nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();

    REQUIRE(ssp4sim_simulator_core_init(nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();
    REQUIRE(ssp4sim_simulator_core_start(nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();
    REQUIRE(ssp4sim_simulator_core_step(nullptr, &status) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();
    REQUIRE(ssp4sim_simulator_core_advance_to(nullptr, 0, &status) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();
    REQUIRE(ssp4sim_simulator_core_get_info(nullptr, &info) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();
    REQUIRE(ssp4sim_simulator_core_finish(nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    require_last_error_set();

    ssp4sim_simulator_core_destroy(nullptr);

    ssp4sim_simulator_core_destroy(handle);
}

TEST_CASE("SimulatorCore C ABI requires init before start and step",
          "[high_level][c_api][simulator_core]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "simulator_core_c_api_state";
    const auto config_path = write_core_config(workdir);

    ssp4sim_simulator_core_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_core_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);

    REQUIRE(ssp4sim_simulator_core_start(handle) == SSP4SIM_STATUS_RUNTIME_ERROR);
    require_last_error_set();

    ssp4sim_step_status status = SSP4SIM_STEP_ADVANCED;
    REQUIRE(ssp4sim_simulator_core_step(handle, &status) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    require_last_error_set();

    REQUIRE(ssp4sim_simulator_core_advance_to(handle, 0, &status) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    require_last_error_set();

    ssp4sim_simulator_core_destroy(handle);
}

TEST_CASE("SimulatorCore C ABI rejects a zero timestep at start",
          "[high_level][c_api][simulator_core]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "simulator_core_c_api_zero";
    const auto config_path = write_core_config(workdir, 0.3, 0.0);

    ssp4sim_simulator_core_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_core_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(handle != nullptr);

    REQUIRE(ssp4sim_simulator_core_init(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_core_start(handle) == SSP4SIM_STATUS_RUNTIME_ERROR);
    require_last_error_set();

    ssp4sim_simulator_core_destroy(handle);
}