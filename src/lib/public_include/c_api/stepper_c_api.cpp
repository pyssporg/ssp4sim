#include "stepper_c_api.h"

#include "c_api/c_api_internal.hpp"

#include "shared_config.hpp"
#include "cpp_api/stepper.hpp"

#include <cstdint>
#include <memory>
#include <utility>

// The C enums must stay numerically identical to the canonical C++ enum in
// cpp_api/stepper.hpp; the ABI is a static_cast, not a translation table.
static_assert(static_cast<int>(ssp4sim::StepStatus::Advanced) ==
                  SSP4SIM_STEP_ADVANCED,
              "ssp4sim_step_status drifts from ssp4sim::StepStatus");
static_assert(static_cast<int>(ssp4sim::StepStatus::Completed) ==
                  SSP4SIM_STEP_COMPLETED,
              "ssp4sim_step_status drifts from ssp4sim::StepStatus");

struct ssp4sim_stepper_handle
{
    ssp4sim::SimulationStepper *stepper;
};

namespace
{
    using ssp4sim::c_api::set_error;
    using ssp4sim::c_api::translate_exceptions;

    ssp4sim::SimulationStepper *stepper_of(ssp4sim_stepper_handle *handle)
    {
        return handle == nullptr ? nullptr : handle->stepper;
    }

    template <typename Fn>
    ssp4sim_status call_stepper(ssp4sim_stepper_handle *handle,
                                const char *action, Fn &&fn)
    {
        auto *stepper = stepper_of(handle);
        if (stepper == nullptr)
        {
            return set_error(
                SSP4SIM_STATUS_INVALID_ARGUMENT,
                "handle must not be null");
        }

        return translate_exceptions(
            action, [stepper, &fn] { fn(*stepper); });
    }
}

ssp4sim_status ssp4sim_stepper_create(const char *config_path,
                                      ssp4sim_stepper_handle **out_handle)
{
    if (out_handle == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "out_handle must not be null");
    }
    *out_handle = nullptr;

    if (config_path == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "config_path must not be null");
    }

    // Build the stepper before the handle so a throwing constructor leaves
    // nothing allocated.
    return translate_exceptions(
        "creating stepper",
        [config_path, out_handle]()
        {
            auto config = ssp4sim::SharedConfig::from_file(config_path);
            auto stepper =
                std::make_unique<ssp4sim::SimulationStepper>(std::move(config));
            auto handle = std::make_unique<ssp4sim_stepper_handle>();
            handle->stepper = stepper.release();
            *out_handle = handle.release();
        });
}

ssp4sim_status ssp4sim_stepper_init(ssp4sim_stepper_handle *handle)
{
    return call_stepper(
        handle,
        "initializing stepper",
        [](ssp4sim::SimulationStepper &stepper) { stepper.init(); });
}

ssp4sim_status ssp4sim_stepper_start(ssp4sim_stepper_handle *handle)
{
    return call_stepper(
        handle,
        "starting stepper",
        [](ssp4sim::SimulationStepper &stepper) { stepper.start(); });
}

ssp4sim_status ssp4sim_stepper_step(ssp4sim_stepper_handle *handle,
                                    ssp4sim_step_status *out_status)
{
    if (out_status == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "out_status must not be null");
    }

    return call_stepper(
        handle,
        "stepping simulation",
        [out_status](ssp4sim::SimulationStepper &stepper)
        {
            *out_status = static_cast<ssp4sim_step_status>(
                static_cast<int>(stepper.step()));
        });
}

ssp4sim_status ssp4sim_stepper_advance_to(ssp4sim_stepper_handle *handle,
                                          uint64_t target_ns,
                                          ssp4sim_step_status *out_status)
{
    if (out_status == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "out_status must not be null");
    }

    return call_stepper(
        handle,
        "advancing simulation",
        [target_ns, out_status](ssp4sim::SimulationStepper &stepper)
        {
            *out_status = static_cast<ssp4sim_step_status>(
                static_cast<int>(stepper.advance_to(target_ns)));
        });
}

ssp4sim_status ssp4sim_stepper_get_info(ssp4sim_stepper_handle *handle,
                                        ssp4sim_stepper_info *out_info)
{
    if (out_info == nullptr)
    {
        return set_error(
            SSP4SIM_STATUS_INVALID_ARGUMENT,
            "out_info must not be null");
    }

    return call_stepper(
        handle,
        "reading stepper info",
        [out_info](ssp4sim::SimulationStepper &stepper)
        {
            out_info->sim_time_ns = stepper.sim_time_ns();
            out_info->end_time_ns = stepper.end_time_ns();
        });
}

ssp4sim_status ssp4sim_stepper_finish(ssp4sim_stepper_handle *handle)
{
    return call_stepper(
        handle,
        "finishing simulation",
        [](ssp4sim::SimulationStepper &stepper) { stepper.finish(); });
}

void ssp4sim_stepper_destroy(ssp4sim_stepper_handle *handle)
{
    if (handle == nullptr)
    {
        return;
    }
    delete handle->stepper;
    delete handle;
}