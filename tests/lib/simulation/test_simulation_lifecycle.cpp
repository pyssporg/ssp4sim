#include "cpp_api/simulator.hpp"
#include "shared_config.hpp"
#include "runtime_control.hpp"
#include "cpp_api/simulator_core.hpp"

#include "ssp4cpp/utils/log.hpp"

#include "reference_ssp_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Description: Verifies the Simulator lifecycle guards:
//                (a) simulate() before init() throws std::runtime_error
//                (b) a second init() throws std::logic_error
// Rationale:   Both callers (Simulator::init, simulator_c_api) call init()
//              exactly once per lifecycle; a second init() is a programming
//              error and must fail loudly instead of silently re-initializing.
// Fixture note: (b) needs a successful first init(), which requires a real
//              SSP. Ssp's ctor takes an archive path (zip or directory);
//              there is no in-memory ctor and no .ssp archive in
//              tests/resources (only .ssd reference files). The test uses the
//              directory-based SSP fixture pattern from
//              test_graph_builder.cpp
//              (resources/reference_ssp/artifacts/models/<name>/<analysis>/<build>)
//              and SKIPs when the fixture is not present, so the suite stays
//              green without a .ssp archive.
// ---------------------------------------------------------------------------

namespace {

    fs::path fixture_path(const std::string &fixture_name)
    {
        return ssp4sim::tests::reference_ssp::fixture_root(fixture_name);
    }

    /// Minimal SharedConfig with recording disabled (csv + sqlite off).
    /// The factory reads required keys (simulation.ssp, start_time, stop_time,
    /// timestep) from the JSON text. The caller supplies the SSP fixture as
    /// the archive path, because Simulator now owns and loads the Ssp itself.
    ssp4sim::SharedConfig make_minimal_config(const fs::path &ssp_dir)
    {
        auto config = ssp4sim::SharedConfig::from_string(R"json(
        {
            "simulation": {
                "ssp": "./fake.ssp",
                "start_time": 0.0,
                "stop_time": 1.0,
                "timestep": 0.1,
                "tolerance": 1e-6,
                "executor": { "forward_derivatives": false },
                "log": {
                    "fmu": false,
                    "level_terminal": "disable",
                    "level_file": "disable",
                    "level_json": "disable",
                    "level_cutelog": "disable"
                },
                "recording": {
                    "csv": {
                        "enable": false
                    },
                    "sqlite": {
                        "enable": false
                    }
                }
            }
        }
        )json", ssp4cpp::utils::log::simple_logger());
        config.enable_recording = false;
        config.csv.enable = false;
        config.sqlite.enable = false;
        config.ssp_path = ssp_dir.string();
        config.working_dir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "sim_lifecycle_test";
        return config;
    }

    /// Fixture directory used by the guard tests; SKIPs when absent.
    fs::path require_scenario_fixture()
    {
        const fs::path ssp_dir = fixture_path("scenario");
        if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
        {
            SKIP("requires an SSP fixture (resources/reference_ssp/artifacts/models/scenario/<analysis>/<build>)");
        }
        return ssp_dir;
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// (a) simulate() before init() throws
// ---------------------------------------------------------------------------
TEST_CASE("simulate before init throws", "[simulation][lifecycle]")
{
    const fs::path ssp_dir = require_scenario_fixture();
    auto config = make_minimal_config(ssp_dir);

    // simulate() must throw on the missing simulation_node guard before
    // touching the loaded graph.
    ssp4sim::Simulator sim(std::move(config));

    try
    {
        sim.simulate();
        FAIL("simulate() before init() should have thrown");
    }
    catch (const std::runtime_error &e)
    {
        REQUIRE(std::string(e.what()).find("before init") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// (b) double init() throws std::logic_error
// ---------------------------------------------------------------------------
TEST_CASE("double init throws logic_error", "[simulation][lifecycle]")
{
    const fs::path ssp_dir = require_scenario_fixture();
    auto config = make_minimal_config(ssp_dir);

    ssp4sim::Simulator sim(std::move(config));

    // First init() must succeed (happy path preserved).
    sim.init();

    // Second init() is a programming error -> must throw std::logic_error.
    REQUIRE_THROWS_AS(sim.init(), std::logic_error);
}

// ---------------------------------------------------------------------------
// (c) happy path: init() + simulate() completes
// ---------------------------------------------------------------------------
TEST_CASE("init then simulate completes", "[simulation][lifecycle]")
{
    const fs::path ssp_dir = require_scenario_fixture();
    auto config = make_minimal_config(ssp_dir);
    config.fmu.start_time = 0;
    config.fmu.end_time = 1;
    config.fmu.timestep = 1;

    ssp4sim::Simulator sim(std::move(config));

    sim.init();
    REQUIRE_NOTHROW(sim.simulate());
}

// ---------------------------------------------------------------------------
// Runtime-control lifecycle integration.
// ---------------------------------------------------------------------------

namespace {

    using ssp4sim::RunResult;
    using ssp4sim::RunState;

    nlohmann::json lifecycle_json(const fs::path &workdir,
                                  const std::string &ssp_path,
                                  double stop_time,
                                  std::optional<double> speed,
                                  bool recording,
                                  double timestep = 0.1)
    {
        nlohmann::json config;
        config["simulation"] = {
            {"ssp", ssp_path},
            {"start_time", 0.0},
            {"stop_time", stop_time},
            {"timestep", timestep},
            {"tolerance", 1e-6},
            {"working_dir", workdir.string()},
            {"executor", {{"method", "jacobi"}, {"forward_derivatives", false}}},
            {"log", {{"fmu", false},
                     {"level_terminal", "disable"},
                     {"level_file", "disable"},
                     {"level_json", "disable"}}},
            {"recording", {{"csv", {{"enable", recording},
                                    {"interval", 0.0},
                                    {"file", (workdir / "result.csv").string()}}},
                           {"sqlite", {{"enable", false}}}}}};

        if (speed.has_value())
        {
            config["simulation"]["speed"] = *speed;
        }

        return config;
    }

    ssp4sim::SharedConfig make_lifecycle_config(const fs::path &workdir,
                                                double stop_time,
                                                std::optional<double> speed,
                                                bool recording,
                                                double timestep = 0.1)
    {
        return ssp4sim::SharedConfig::from_json(
            lifecycle_json(workdir, "./fake.ssp", stop_time, speed, recording,
                           timestep),
            ssp4cpp::utils::log::simple_logger());
    }

    fs::path write_config_file(const fs::path &workdir,
                               const std::string &ssp_path,
                               double stop_time,
                               std::optional<double> speed,
                               bool recording,
                               double timestep = 0.1)
    {
        fs::create_directories(workdir);
        const auto path = workdir / "config.json";
        std::ofstream output(path, std::ios::trunc);
        REQUIRE(output.is_open());
        output << lifecycle_json(workdir, ssp_path, stop_time, speed, recording,
                                 timestep)
                      .dump(2);
        return path;
    }

    bool wait_until(const std::function<bool()> &predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }

    std::string trim(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::vector<std::vector<std::string>> read_csv(const fs::path &path)
    {
        std::ifstream input(path);
        REQUIRE(input.is_open());

        std::vector<std::vector<std::string>> rows;
        std::string line;
        while (std::getline(input, line))
        {
            std::vector<std::string> row;
            std::istringstream stream(line);
            std::string cell;
            while (std::getline(stream, cell, ','))
            {
                row.push_back(trim(cell));
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

} // anonymous namespace

TEST_CASE("start then wait completes one run", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "start_wait";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));

    sim.init();
    sim.start();
    REQUIRE(sim.wait() == RunResult::Completed);
    REQUIRE(sim.info().state == RunState::Completed);
}

TEST_CASE("a second start is rejected", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "double_start";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));

    sim.init();
    sim.start();
    REQUIRE_THROWS_AS(sim.start(), std::logic_error);
    REQUIRE(sim.wait() == RunResult::Completed);
}

TEST_CASE("invalid control transitions throw logic_error",
          "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "invalid_transitions";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    REQUIRE(sim.info().state == RunState::Initialized);

    REQUIRE_THROWS_AS(sim.pause(), std::logic_error);
    REQUIRE_THROWS_AS(sim.resume(), std::logic_error);
    REQUIRE_THROWS_AS(sim.stop(), std::logic_error);
    REQUIRE(sim.info().state == RunState::Initialized);

    REQUIRE_THROWS_AS(sim.set_speed(-1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(
        sim.set_speed(std::numeric_limits<double>::quiet_NaN()),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        sim.set_speed(std::numeric_limits<double>::infinity()),
        std::invalid_argument);

    REQUIRE_NOTHROW(sim.set_speed(2.0));
    REQUIRE(sim.info().speed == 2.0);
    REQUIRE(sim.info().state == RunState::Initialized);
}

TEST_CASE("immediate pause resume stop and speed stay coherent",
          "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "immediate_control";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 10.0, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.set_speed(2.0);
    REQUIRE(sim.info().speed == 2.0);
    sim.set_speed(4.0);
    REQUIRE(sim.info().speed == 4.0);

    sim.pause();
    REQUIRE(sim.info().state == RunState::Paused);
    REQUIRE(sim.info().speed == 4.0);

    sim.resume();
    REQUIRE(sim.info().state == RunState::Running);

    sim.stop();
    REQUIRE((sim.info().state == RunState::Stopping ||
             sim.info().state == RunState::Stopped));
    REQUIRE(sim.wait() == RunResult::Stopped);
    REQUIRE(sim.info().state == RunState::Stopped);

    // Stopping and terminal states are idempotent; stop never overwrites them.
    REQUIRE_NOTHROW(sim.stop());
    REQUIRE(sim.info().state == RunState::Stopped);
}

TEST_CASE("scheduled pause fires at its simulation time",
          "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "pause_scheduled";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 5.0, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.pause(200'000'000);
    REQUIRE(sim.info().state == RunState::Running);
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Paused; }));

    sim.resume();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.stop();
    REQUIRE(sim.wait() == RunResult::Stopped);
}

TEST_CASE("scheduled speed change applies and last equal-timestamp wins",
          "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "speed_scheduled";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 0.5, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.set_speed(2.0, 200'000'000);
    sim.set_speed(4.0, 200'000'000);

    REQUIRE(sim.wait() == RunResult::Completed);
    REQUIRE(sim.info().speed == 4.0);
}

TEST_CASE("pause then resume completes the run", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "pause_resume";
    auto config = make_lifecycle_config(workdir, 0.2, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));

    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.pause();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Paused; }));

    sim.resume();
    REQUIRE(sim.wait() == RunResult::Completed);
    REQUIRE(sim.info().state == RunState::Completed);
}

TEST_CASE("stop yields Stopped and flushes partial CSV", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "stop_partial";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 1.0, 1.0, true, 0.1);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));

    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    // Let a couple of macro steps run so the stopped run has partial data.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    sim.stop();

    REQUIRE(sim.wait() == RunResult::Stopped);
    REQUIRE(sim.info().state == RunState::Stopped);
    REQUIRE(fs::exists(workdir / "result.csv"));

    const auto rows = read_csv(workdir / "result.csv");
    REQUIRE(rows.size() >= 2);
}

TEST_CASE("destructor while running stops and joins", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "destructor_running";
    auto config = make_lifecycle_config(workdir, 0.5, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    // Simulator owns the Ssp now, so destroying it while running must request
    // stop, wait, and join before its own engine members are torn down.
    {
        ssp4sim::Simulator sim(std::move(config));
        sim.init();
        sim.start();
        REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));
    } // request stop, wait and join must complete without throwing
}

TEST_CASE("scheduled stop stops at the loop top", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "stop_scheduled";
    fs::remove_all(workdir);

    // Paced at real time so the future stop can be queued while the run is
    // still well before the requested stop time.
    const auto config_path = write_config_file(workdir, ssp_dir.string(), 1.0,
                                               1.0, false, 0.01);

    ssp4sim::Simulator simulator(config_path.string());
    simulator.init();
    simulator.start();
    REQUIRE(wait_until([&]() { return simulator.info().state == ssp4sim::RunState::Running; }));

    const auto at_ns = static_cast<uint64_t>(0.2 * 1.0e9);
    simulator.stop(at_ns);

    REQUIRE(simulator.wait() == ssp4sim::RunResult::Stopped);
    REQUIRE(simulator.info().state == ssp4sim::RunState::Stopped);
}

TEST_CASE("runner pre-run shutdown yields Stopped", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "pre_run_stop";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    runner.shutdown();
    REQUIRE(runner.info().state == RunState::Stopped);
    runner.simulate();
    REQUIRE(runner.info().state == RunState::Stopped);
}

TEST_CASE("completed run ignores shutdown", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "completed_stop";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    runner.simulate();
    REQUIRE(runner.info().state == RunState::Completed);

    // Terminal states are monotonic: stop and shutdown leave Completed alone.
    REQUIRE_NOTHROW(runner.stop());
    REQUIRE(runner.info().state == RunState::Completed);

    runner.shutdown();
    REQUIRE(runner.info().state == RunState::Completed);
}

TEST_CASE("completed run publishes final sim time", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "final_time";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 1.0);
    config.ssp_path = ssp_dir.string();
    const uint64_t end_time = config.fmu.end_time;

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    runner.simulate();
    REQUIRE(runner.info().state == RunState::Completed);
    REQUIRE(runner.info().sim_time_ns == end_time);
}

TEST_CASE("zero timestep is rejected by the runner loop", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "zero_timestep";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 0.0);
    config.ssp_path = ssp_dir.string();
    REQUIRE(config.fmu.timestep == 0);

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    REQUIRE_THROWS_AS(runner.simulate(), std::runtime_error);
    REQUIRE(runner.info().state == RunState::Failed);
}

TEST_CASE("scheduled stop at end_time yields Stopped", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "stop_at_end";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 0.5, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();
    const uint64_t end_time = config.fmu.end_time;

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.stop(end_time);
    REQUIRE(sim.wait() == RunResult::Stopped);
    REQUIRE(sim.info().state == RunState::Stopped);
    REQUIRE(sim.info().sim_time_ns == end_time);
}

TEST_CASE("stop while paused at current time stops without resume", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("signal_step_gain");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the signal_step_gain SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "paused_stop";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 1.0, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.pause();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Paused; }));

    const uint64_t at_ns = sim.info().sim_time_ns;
    sim.stop(at_ns);

    REQUIRE(sim.wait() == RunResult::Stopped);
    REQUIRE(sim.info().state == RunState::Stopped);
}

TEST_CASE("reset discards a live run and rebuilds from the creation config", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "reset_live";
    fs::remove_all(workdir);

    auto config = make_lifecycle_config(workdir, 0.2, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();
    const uint64_t start_time = config.fmu.start_time;

    ssp4sim::Simulator sim(std::move(config));
    sim.init();
    sim.start();
    REQUIRE(wait_until([&]() { return sim.info().state == RunState::Running; }));

    sim.reset();

    REQUIRE(sim.info().state == RunState::Initialized);
    REQUIRE(sim.info().sim_time_ns == start_time);

    sim.start();
    REQUIRE(sim.wait() == RunResult::Completed);
}

TEST_CASE("immediate shutdown after start yields Stopped", "[simulation][lifecycle][control]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "immediate_teardown";
    auto config = make_lifecycle_config(workdir, 1.0, 1.0, false, 0.01);
    config.ssp_path = ssp_dir.string();

    ssp4sim::Simulator runner(std::move(config));
    runner.init();

    runner.start();
    runner.shutdown();

    REQUIRE(runner.info().state == RunState::Stopped);
}

// ---------------------------------------------------------------------------
// Synchronous core: no thread, pacing or control channel involved.
// ---------------------------------------------------------------------------

TEST_CASE("SimulatorCore advances to an exact time and finishes", "[simulation][simulator_core]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "simulator_core_sync";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 0.1);
    config.ssp_path = ssp_dir.string();

    const uint64_t end_time = config.fmu.end_time;
    REQUIRE(end_time > 0);

    ssp4sim::SimulatorCore stepper(std::move(config));

    REQUIRE_FALSE(stepper.initialized());
    stepper.init();
    REQUIRE(stepper.initialized());
    REQUIRE(stepper.sim_time_ns() == 0);
    REQUIRE(stepper.end_time_ns() == end_time);

    stepper.start();

    const uint64_t target_ns = end_time / 2 + end_time / 20;
    REQUIRE(target_ns < end_time);
    REQUIRE(stepper.advance_to(target_ns) == ssp4sim::StepStatus::Advanced);
    REQUIRE(stepper.sim_time_ns() == target_ns);

    REQUIRE(stepper.advance_to(end_time * 2) == ssp4sim::StepStatus::Completed);
    REQUIRE(stepper.sim_time_ns() == end_time);
    REQUIRE(stepper.step() == ssp4sim::StepStatus::Completed);

    REQUIRE_NOTHROW(stepper.finish());
    REQUIRE_NOTHROW(stepper.finish());
    REQUIRE_THROWS_AS(stepper.step(), std::logic_error);
}

TEST_CASE("SimulatorCore rejects a zero timestep at start", "[simulation][simulator_core]")
{
    const fs::path ssp_dir = fixture_path("scenario");
    if (!fs::exists(ssp_dir / "SystemStructure.ssd"))
    {
        SKIP("requires the scenario SSP fixture");
    }

    auto workdir = ssp4sim::tests::reference_ssp::project_root() / ".dynamic-harness" / "wd" / "simulator_core_zero";
    auto config = make_lifecycle_config(workdir, 1.0, std::nullopt, false, 0.0);
    config.ssp_path = ssp_dir.string();
    REQUIRE(config.fmu.timestep == 0);

    ssp4sim::SimulatorCore stepper(std::move(config));
    stepper.init();

    REQUIRE_THROWS_AS(stepper.start(), std::runtime_error);
}
