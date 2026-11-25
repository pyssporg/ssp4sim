#include <catch2/catch_test_macros.hpp>

#include "simulator_c_api.h"

#include "simulator.hpp"

#include "reference_ssp_fixture.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

namespace
{
    namespace fs = std::filesystem;

    fs::path write_control_config(const fs::path &workdir,
                                  double stop_time = 10.0,
                                  double speed = 1.0)
    {
        const auto ssp_root = ssp4sim::tests::reference_ssp::fixture_root(
            "signal_step_gain");
        REQUIRE(fs::exists(ssp_root / "SystemStructure.ssd"));

        fs::create_directories(workdir);

        nlohmann::json config;
        config["simulation"] = {
            {"ssp", ssp_root.string()},
            {"start_time", 0.0},
            {"stop_time", stop_time},
            {"timestep", 0.05},
            {"tolerance", 1e-6},
            {"speed", speed},
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

    bool wait_for_state(ssp4sim_simulator_handle *handle,
                        const ssp4sim_run_state expected,
                        const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (ssp4sim_simulator_get_info(handle).state == expected)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return ssp4sim_simulator_get_info(handle).state == expected;
    }
}

// ---------------------------------------------------------------------------
// Description: Exercises the C ABI runtime-control lifecycle on a small
//              reference SSP: init, start, pause, resume, stop, wait, info
//              and set_speed validation.
// Rationale:   The Python bindings are a thin layer over this ABI; the C
//              surface must expose the same lifecycle before Python can.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI runtime control lifecycle", "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_control";
    const auto config_path = write_control_config(workdir);

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(handle != nullptr);

    ssp4sim_run_info info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_CREATED);

    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);
    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_INITIALIZED);

    REQUIRE(info.speed == 1.0);

    REQUIRE(ssp4sim_simulator_set_speed(handle, -1.0, 0, 0) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(ssp4sim_last_error() != nullptr);
    REQUIRE(ssp4sim_last_error()[0] != '\0');
    REQUIRE(ssp4sim_simulator_set_speed(
                handle, std::numeric_limits<double>::infinity(), 0, 0) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);

    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_RUNNING,
                           std::chrono::seconds(5)));

    // A scheduled pause must be forwarded, not applied immediately. At speed
    // 1.0 one simulated second is about one wall-clock second, so the state is
    // still RUNNING right after the call.
    const uint64_t scheduled_pause_ns = 1000000000ULL;
    REQUIRE(ssp4sim_simulator_pause(handle, 1, scheduled_pause_ns) ==
            SSP4SIM_STATUS_OK);
    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_RUNNING);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_PAUSED,
                           std::chrono::seconds(5)));

    // The coherent info agrees with the live view while the run is in progress.
    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.speed == 1.0);
    REQUIRE(info.sim_time_ns > 0);
    REQUIRE(info.sim_time_ns <= 10000000000ULL);

    REQUIRE(ssp4sim_simulator_resume(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_RUNNING,
                           std::chrono::seconds(5)));

    REQUIRE(ssp4sim_simulator_pause(handle, 0, 0) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_PAUSED,
                           std::chrono::seconds(5)));

    REQUIRE(ssp4sim_simulator_resume(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_RUNNING,
                           std::chrono::seconds(5)));

    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) == SSP4SIM_STATUS_OK);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_STOPPED);
    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_STOPPED);

    // Partial results stay readable after a stopped run.
    REQUIRE(info.sim_time_ns > 0);
    REQUIRE(info.sim_time_ns <= 10000000000ULL);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: wait() reports a completed run with SSP4SIM_RESULT_COMPLETED.
// Rationale:   The terminal outcome must reach the caller instead of being
//              reconstructed from the state machine.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI wait reports completed", "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_completed";
    const auto config_path = write_control_config(workdir, 0.3, 0.0);

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_COMPLETED);

    const ssp4sim_run_info info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_COMPLETED);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: get_info() returns one coherent RunInfo that matches the C++
//              reading, and stop() stays OK after the run is terminal.
// Rationale:   get_info() is the sole readback; the coherent read must not
//              invent a second state policy.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI coherent info and stop after terminal",
          "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_info";
    const auto config_path = write_control_config(workdir, 0.3, 0.0);

    ssp4sim::RunInfo reference;
    {
        ssp4sim::Simulator simulator(config_path.string());
        simulator.init();
        simulator.simulate();
        reference = simulator.info();
    }

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_COMPLETED);

    const ssp4sim_run_info info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_COMPLETED);
    REQUIRE(info.sim_time_ns == reference.sim_time_ns);
    REQUIRE(info.speed == reference.speed);

    // stop() is a no-op after a terminal state and must not overwrite it.
    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_stop(handle, 1, 0) == SSP4SIM_STATUS_OK);

    const ssp4sim_run_info terminal = ssp4sim_simulator_get_info(handle);
    REQUIRE(terminal.state == SSP4SIM_STATE_COMPLETED);

    const ssp4sim_run_info after_stop = ssp4sim_simulator_get_info(handle);
    REQUIRE(after_stop.state == SSP4SIM_STATE_COMPLETED);
    REQUIRE(after_stop.sim_time_ns == info.sim_time_ns);
    REQUIRE(after_stop.speed == info.speed);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: stop() is idempotent while the run is Stopping.
// Rationale:   A second stop must not turn a legitimate teardown into an
//              error, regardless of whether the run thread has finished yet.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI stop is idempotent once stopping",
          "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_stop_twice";
    const auto config_path = write_control_config(workdir, 10.0, 1.0);

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);

    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) == SSP4SIM_STATUS_OK);
    // The run is Stopping until the run thread observes the request; a second
    // stop must also succeed if it already reached Stopped.
    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) == SSP4SIM_STATUS_OK);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_STOPPED);

    const ssp4sim_run_info stopped = ssp4sim_simulator_get_info(handle);
    REQUIRE(stopped.state == SSP4SIM_STATE_STOPPED);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: The C get_info() reproduces the C++ simulator's own readings for
//              the same configuration.
// Rationale:   It must be a thin projection of Simulator::info(), not a second
//              state policy.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI info agrees with C++", "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_getters";
    const auto config_path = write_control_config(workdir, 0.3, 1.0);

    ssp4sim::RunInfo reference;
    {
        ssp4sim::Simulator simulator(config_path.string());
        simulator.init();
        simulator.simulate();
        reference = simulator.info();
    }

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);

    ssp4sim_run_info info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.speed == reference.speed);

    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_RUNNING,
                           std::chrono::seconds(5)));
    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.speed == reference.speed);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_COMPLETED);

    info = ssp4sim_simulator_get_info(handle);
    REQUIRE(info.state == SSP4SIM_STATE_COMPLETED);
    REQUIRE(info.sim_time_ns == reference.sim_time_ns);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: A run that cannot produce an outcome surfaces an error status
//              and invalid transitions map to RUNTIME_ERROR.
// Rationale:   wait() must never report a failed run as completed, and
//              std::logic_error from RunControl must not be flattened into
//              INVALID_ARGUMENT.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI wait surfaces run errors", "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_errors";
    const auto config_path = write_control_config(workdir, 10.0, 1.0);

    ssp4sim_simulator_handle *handle = nullptr;
    REQUIRE(ssp4sim_simulator_create(config_path.string().c_str(), &handle) ==
            SSP4SIM_STATUS_OK);

    ssp4sim_run_result result = SSP4SIM_RESULT_COMPLETED;
    REQUIRE(ssp4sim_simulator_wait(handle, &result) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    REQUIRE(ssp4sim_last_error()[0] != '\0');
    REQUIRE(ssp4sim_simulator_wait(handle, nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);

    REQUIRE(ssp4sim_simulator_init(handle) == SSP4SIM_STATUS_OK);

    // Invalid transitions: resume/pause need Paused/Running, and wait() needs
    // a started run. stop() stays an error before the run starts.
    REQUIRE(ssp4sim_simulator_resume(handle) == SSP4SIM_STATUS_RUNTIME_ERROR);
    REQUIRE(ssp4sim_simulator_pause(handle, 0, 0) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    REQUIRE(ssp4sim_simulator_wait(handle, &result) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);

    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_OK);
    REQUIRE(wait_for_state(handle, SSP4SIM_STATE_RUNNING,
                           std::chrono::seconds(5)));
    REQUIRE(ssp4sim_simulator_start(handle) == SSP4SIM_STATUS_RUNTIME_ERROR);

    REQUIRE(ssp4sim_simulator_stop(handle, 0, 0) == SSP4SIM_STATUS_OK);
    REQUIRE(ssp4sim_simulator_wait(handle, &result) == SSP4SIM_STATUS_OK);
    REQUIRE(result == SSP4SIM_RESULT_STOPPED);

    ssp4sim_simulator_destroy(handle);
}

// ---------------------------------------------------------------------------
// Description: create() reports a missing config without leaking a handle.
// Rationale:   A throwing Simulator constructor used to leak the handle that
//              had already been allocated.
// ---------------------------------------------------------------------------
TEST_CASE("C ABI create error path", "[high_level][c_api][control]")
{
    const auto workdir = ssp4sim::tests::reference_ssp::project_root() /
                         "build" / "test_cpp_high_level" / "c_api_create";
    fs::create_directories(workdir);
    const auto missing_config = workdir / "missing_config.json";
    REQUIRE_FALSE(fs::exists(missing_config));

    ssp4sim_simulator_handle *handle = reinterpret_cast<
        ssp4sim_simulator_handle *>(0x1);
    REQUIRE(ssp4sim_simulator_create(missing_config.string().c_str(), &handle) ==
            SSP4SIM_STATUS_RUNTIME_ERROR);
    REQUIRE(handle == nullptr);
    REQUIRE(ssp4sim_last_error()[0] != '\0');

    REQUIRE(ssp4sim_simulator_create(nullptr, &handle) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(handle == nullptr);

    REQUIRE(ssp4sim_simulator_create(missing_config.string().c_str(), nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
}

TEST_CASE("C ABI rejects null control arguments", "[high_level][c_api][control]")
{
    REQUIRE(ssp4sim_simulator_start(nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(ssp4sim_simulator_pause(nullptr, 0, 0) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(ssp4sim_simulator_resume(nullptr) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);
    REQUIRE(ssp4sim_simulator_stop(nullptr, 0, 0) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);

    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    REQUIRE(ssp4sim_simulator_wait(nullptr, &result) ==
            SSP4SIM_STATUS_INVALID_ARGUMENT);

    const ssp4sim_run_info null_info = ssp4sim_simulator_get_info(nullptr);
    REQUIRE(null_info.state == SSP4SIM_STATE_CREATED);
    REQUIRE(null_info.sim_time_ns == 0);
    REQUIRE(null_info.speed == 0.0);
    REQUIRE(ssp4sim_last_error() != nullptr);
    REQUIRE(ssp4sim_last_error()[0] != '\0');
}
