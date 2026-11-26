
#include <catch2/catch_test_macros.hpp>

#include "utils/config.hpp"
#include "shared_config.hpp"

#include "ssp4cpp/utils/log.hpp"

#include <filesystem>
#include <string>

using namespace ssp4sim::utils;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Description: Config value-type tests covering file loading, missing/malformed
//              files, required values, defaults, type mismatches, path
//              resolution, and recording config parsing.
// Rationale:   Config is the primary user-facing interface for simulation setup.
// ---------------------------------------------------------------------------
TEST_CASE("Config tests", "[config]")
{
    const fs::path project_root{SSP4SIM_PROJECT_ROOT};
    const std::string config_file = (project_root / "tests" / "resources" / "references" / "test_config.json").string();
    const std::string malformed_config_file = (project_root / "tests" / "resources"  / "references" / "malformed_test_config.json").string();

    SECTION("Load from file")
    {
        REQUIRE_NOTHROW(Config::fromFile(config_file));
    }

    SECTION("Load from non-existent file")
    {
        REQUIRE_THROWS(Config::fromFile("non_existent_file.json"));
    }

    SECTION("Load from malformed file")
    {
        REQUIRE_THROWS(Config::fromFile(malformed_config_file));
    }

    SECTION("Get required value")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE(cfg.getString("name") == "test");
        REQUIRE(cfg.getInt("value") == 123);
        REQUIRE(cfg.getString("nested.key") == "value");
        REQUIRE(cfg.getString("array.0") == "a");
    }

    SECTION("Get required value that does not exist")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE_THROWS(cfg.getString("non_existent_key"));
    }

    SECTION("Get required value with type mismatch")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE_THROWS(cfg.getInt("name"));
    }

    SECTION("Get with default value")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE(cfg.getOr("name", std::string("default")) == "test");
        REQUIRE(cfg.getOr("value", 456) == 123);
        REQUIRE(cfg.getOr("nested.key", std::string("default")) == "value");
        REQUIRE(cfg.getOr("array.0", std::string("default")) == "a");
    }

    SECTION("Get with default value that does not exist")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE(cfg.getOr("non_existent_key", std::string("default")) == "default");
    }

    SECTION("Get with default value with type mismatch")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE_THROWS(cfg.getOr("name", 456));
    }

    SECTION("Resolve path")
    {
        const auto cfg = Config::fromFile(config_file);
        REQUIRE(cfg.resolvePath("name") != nullptr);
        REQUIRE(cfg.resolvePath("nested.key") != nullptr);
        REQUIRE(cfg.resolvePath("array.0") != nullptr);
        REQUIRE(cfg.resolvePath("non_existent_key") == nullptr);
    }

    SECTION("Recording config is parsed")
    {
        auto *log = ssp4cpp::utils::log::simple_logger();
        const auto shared_config = ssp4sim::SharedConfig::from_string(R"json({
            "simulation": {
                "ssp": "./fake.ssp",
                "working_dir": "./wd/test",
                "start_time": 0.0,
                "stop_time": 1.0,
                "timestep": 0.1,
                "tolerance": 1e-6,
                "recording": {
                    "csv": {
                        "enable": true,
                        "interval": 0.5
                    }
                }
            }
        })json", log);

        REQUIRE(shared_config.enable_recording);
        REQUIRE(shared_config.csv.enable);
        REQUIRE(shared_config.csv.file == fs::path("./wd/test/result.csv"));
        REQUIRE(shared_config.csv.interval == ssp4sim::utils::time::s_to_ns(0.5));
    }

    SECTION("Recording config can be disabled")
    {
        auto *log = ssp4cpp::utils::log::simple_logger();
        const auto shared_config = ssp4sim::SharedConfig::from_string(R"json(
        {
            "simulation": {
                "ssp": "./fake.ssp",
                "start_time": 0.0,
                "stop_time": 1.0,
                "timestep": 0.1,
                "tolerance": 1e-6,
                "recording": {
                    "csv": {
                        "enable": false
                    }
                }
            }
        }
        )json", log);

        REQUIRE_FALSE(shared_config.enable_recording);
        REQUIRE_FALSE(shared_config.csv.enable);
    }

}
