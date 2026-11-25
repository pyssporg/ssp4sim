#include "simulator_c_api.h"

#include "simulator.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

// The C enums must stay numerically identical to the canonical C++ enums in
// runtime_control.hpp; the ABI is a static_cast, not a translation table.
static_assert(static_cast<int>(ssp4sim::RunState::Created) == SSP4SIM_STATE_CREATED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Initialized) == SSP4SIM_STATE_INITIALIZED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Running) == SSP4SIM_STATE_RUNNING,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Paused) == SSP4SIM_STATE_PAUSED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Stopping) == SSP4SIM_STATE_STOPPING,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Completed) == SSP4SIM_STATE_COMPLETED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Stopped) == SSP4SIM_STATE_STOPPED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");
static_assert(static_cast<int>(ssp4sim::RunState::Failed) == SSP4SIM_STATE_FAILED,
              "ssp4sim_run_state drifts from ssp4sim::RunState");

static_assert(static_cast<int>(ssp4sim::RunResult::Completed) == SSP4SIM_RESULT_COMPLETED,
              "ssp4sim_run_result drifts from ssp4sim::RunResult");
static_assert(static_cast<int>(ssp4sim::RunResult::Stopped) == SSP4SIM_RESULT_STOPPED,
              "ssp4sim_run_result drifts from ssp4sim::RunResult");
static_assert(static_cast<int>(ssp4sim::RunResult::Failed) == SSP4SIM_RESULT_FAILED,
              "ssp4sim_run_result drifts from ssp4sim::RunResult");

struct ssp4sim_simulator_handle
{
    ssp4sim::Simulator *simulator;
};

namespace
{
    thread_local std::string g_last_error;

    ssp4sim_status set_error(const ssp4sim_status status,
                             const std::string &message)
    {
        g_last_error = message;
        return status;
    }

    ssp4sim::Simulator *simulator_of(ssp4sim_simulator_handle *handle)
    {
        return handle == nullptr ? nullptr : handle->simulator;
    }

    std::optional<uint64_t> to_at_sim_time(int has_at_sim_time,
                                           uint64_t at_sim_time_ns)
    {
        if (has_at_sim_time)
        {
            return at_sim_time_ns;
        }
        return std::nullopt;
    }

    // The single translation from C++ error types to C status codes.
    template <typename Fn>
    ssp4sim_status translate_exceptions(const char *action, Fn &&fn)
    {
        try
        {
            fn();
            g_last_error.clear();
            return SSP4SIM_STATUS_OK;
        }
        catch (const std::invalid_argument &ex)
        {
            return set_error(SSP4SIM_STATUS_INVALID_ARGUMENT, ex.what());
        }
        catch (const std::logic_error &ex)
        {
            return set_error(SSP4SIM_STATUS_RUNTIME_ERROR, ex.what());
        }
        catch (const std::bad_alloc &)
        {
            return set_error(SSP4SIM_STATUS_OUT_OF_MEMORY,
                             "failed to allocate memory");
        }
        catch (const std::exception &ex)
        {
            return set_error(SSP4SIM_STATUS_RUNTIME_ERROR, ex.what());
        }
        catch (...)
        {
            return set_error(
                SSP4SIM_STATUS_RUNTIME_ERROR,
                std::string("Unknown error while ") + action);
        }
    }

    template <typename Fn>
    ssp4sim_status call_simulator(ssp4sim_simulator_handle *handle,
                                  const char *action, Fn &&fn)
    {
        auto *simulator = simulator_of(handle);
        if (simulator == nullptr)
        {
            return set_error(
                SSP4SIM_STATUS_INVALID_ARGUMENT,
                "handle must not be null");
        }

        return translate_exceptions(
            action, [simulator, &fn] { fn(*simulator); });
    }

}

ssp4sim_status ssp4sim_simulator_create(
    const char *config_path,
    ssp4sim_simulator_handle **out_handle
)
{
    if (out_handle == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "out_handle must not be null"
        );
    }
    *out_handle = nullptr;

    if (config_path == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "config_path must not be null"
        );
    }

    // Build the simulator before the handle so a throwing constructor leaves
    // nothing allocated.
    return translate_exceptions(
        "creating simulator",
        [config_path, out_handle]()
        {
            auto simulator =
                std::make_unique<ssp4sim::Simulator>(std::string(config_path));
            auto handle = std::make_unique<ssp4sim_simulator_handle>();
            handle->simulator = simulator.release();
            *out_handle = handle.release();
        });
}

ssp4sim_status ssp4sim_simulator_init(ssp4sim_simulator_handle *handle)
{
    return call_simulator(
        handle,
        "initializing simulator",
        [](ssp4sim::Simulator &simulator) { simulator.init(); });
}

ssp4sim_status ssp4sim_simulator_simulate(ssp4sim_simulator_handle *handle)
{
    return call_simulator(
        handle,
        "running simulation",
        [](ssp4sim::Simulator &simulator) { simulator.simulate(); });
}

ssp4sim_status ssp4sim_simulator_start(ssp4sim_simulator_handle *handle)
{
    return call_simulator(
        handle,
        "starting simulator",
        [](ssp4sim::Simulator &simulator) { simulator.start(); });
}

ssp4sim_status ssp4sim_simulator_pause(ssp4sim_simulator_handle *handle, int has_at_sim_time,
                            uint64_t at_sim_time_ns)
{
    return call_simulator(
        handle,
        "pausing simulator",
        [has_at_sim_time, at_sim_time_ns](ssp4sim::Simulator &simulator)
        {
            simulator.pause(
                to_at_sim_time(has_at_sim_time, at_sim_time_ns));
        });
}

ssp4sim_status ssp4sim_simulator_resume(ssp4sim_simulator_handle *handle)
{
    return call_simulator(
        handle,
        "resuming simulator",
        [](ssp4sim::Simulator &simulator) { simulator.resume(); });
}

ssp4sim_status ssp4sim_simulator_stop(ssp4sim_simulator_handle *handle, int has_at_sim_time,
                           uint64_t at_sim_time_ns)
{
    return call_simulator(
        handle,
        "stopping simulator",
        [has_at_sim_time, at_sim_time_ns](ssp4sim::Simulator &simulator)
        {
            simulator.stop(
                to_at_sim_time(has_at_sim_time, at_sim_time_ns));
        });
}

ssp4sim_status ssp4sim_simulator_set_speed(ssp4sim_simulator_handle *handle, double speed,
                                int has_at_sim_time, uint64_t at_sim_time_ns)
{
    return call_simulator(
        handle,
        "setting speed",
        [speed, has_at_sim_time, at_sim_time_ns](ssp4sim::Simulator &simulator)
        {
            simulator.set_speed(
                speed, to_at_sim_time(has_at_sim_time, at_sim_time_ns));
        });
}

ssp4sim_status ssp4sim_simulator_wait(ssp4sim_simulator_handle *handle,
                                      ssp4sim_run_result *result)
{
    if (result == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "result must not be null");
    }

    // wait() returns Completed/Stopped and throws when the run failed, so a
    // failed run reaches the caller as RUNTIME_ERROR, never as a result.
    return call_simulator(
        handle,
        "waiting for simulation",
        [result](ssp4sim::Simulator &simulator)
        {
            *result = static_cast<ssp4sim_run_result>(
                static_cast<int>(simulator.wait()));
        });
}

ssp4sim_run_info ssp4sim_simulator_get_info(
    ssp4sim_simulator_handle *handle)
{
    ssp4sim_run_info info{};

    auto *simulator = simulator_of(handle);
    if (simulator == nullptr)
    {
        set_error(SSP4SIM_STATUS_INVALID_ARGUMENT, "handle must not be null");
        return info;
    }

    translate_exceptions(
        "reading simulator info",
        [simulator, &info]()
        {
            const ssp4sim::RunInfo run_info = simulator->info();
            info.state = static_cast<ssp4sim_run_state>(
                static_cast<int>(run_info.state));
            info.sim_time_ns = run_info.sim_time_ns;
            info.speed = run_info.speed;
        });

    return info;
}

void ssp4sim_simulator_destroy(ssp4sim_simulator_handle *handle)
{
    if (handle == nullptr)
    {
        return;
    }
    delete handle->simulator;
    delete handle;
}

const char *ssp4sim_last_error(void)
{
    return g_last_error.c_str();
}
