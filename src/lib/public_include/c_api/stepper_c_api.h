#pragma once

#include "c_api_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssp4sim_stepper_handle ssp4sim_stepper_handle;

typedef enum
{
    SSP4SIM_STEP_ADVANCED = 0,
    SSP4SIM_STEP_COMPLETED = 1
} ssp4sim_step_status;

typedef struct
{
    uint64_t sim_time_ns;
    uint64_t end_time_ns;
} ssp4sim_stepper_info;

/* Canonical enum <-> string spellings, header-only so no new symbols. */
static inline const char *ssp4sim_step_status_name(ssp4sim_step_status status)
{
    switch (status)
    {
    case SSP4SIM_STEP_ADVANCED:
        return "advanced";
    case SSP4SIM_STEP_COMPLETED:
        return "completed";
    }
    return "completed";
}

ssp4sim_status ssp4sim_stepper_create(const char *config_path,
                                      ssp4sim_stepper_handle **out_handle);
ssp4sim_status ssp4sim_stepper_init(ssp4sim_stepper_handle *handle);
ssp4sim_status ssp4sim_stepper_start(ssp4sim_stepper_handle *handle);

/* Advance one macro step and write the outcome. */
ssp4sim_status ssp4sim_stepper_step(ssp4sim_stepper_handle *handle,
                                    ssp4sim_step_status *out_status);

/* Advance to an exact simulation time, clamped to the configured end time.
 * Never sleeps or paces. */
ssp4sim_status ssp4sim_stepper_advance_to(ssp4sim_stepper_handle *handle,
                                          uint64_t target_ns,
                                          ssp4sim_step_status *out_status);

ssp4sim_status ssp4sim_stepper_get_info(ssp4sim_stepper_handle *handle,
                                        ssp4sim_stepper_info *out_info);

/* Stop and flush the recorder. Idempotent. */
ssp4sim_status ssp4sim_stepper_finish(ssp4sim_stepper_handle *handle);

/* Releases the handle without calling finish(). Callers must call
 * ssp4sim_stepper_finish() first to flush recording. A null handle is a
 * no-op. */
void ssp4sim_stepper_destroy(ssp4sim_stepper_handle *handle);

#ifdef __cplusplus
}
#endif