#pragma once

#include "c_api_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssp4sim_simulator_handle ssp4sim_simulator_handle;

typedef enum
{
    SSP4SIM_STATE_CREATED = 0,
    SSP4SIM_STATE_INITIALIZED = 1,
    SSP4SIM_STATE_RUNNING = 2,
    SSP4SIM_STATE_PAUSED = 3,
    SSP4SIM_STATE_STOPPING = 4,
    SSP4SIM_STATE_COMPLETED = 5,
    SSP4SIM_STATE_STOPPED = 6,
    SSP4SIM_STATE_FAILED = 7
} ssp4sim_run_state;

typedef enum
{
    SSP4SIM_RESULT_COMPLETED = 0,
    SSP4SIM_RESULT_STOPPED = 1,
    SSP4SIM_RESULT_FAILED = 2
} ssp4sim_run_result;

/* Coherent view of the run-control plane. state, sim_time_ns and speed are
 * read together; get_info() is the sole readback. */
typedef struct
{
    ssp4sim_run_state state;
    uint64_t sim_time_ns;
    double speed;
} ssp4sim_run_info;

/* Canonical enum <-> string spellings shared by the C API, the daemon, and the
 * Python bindings. Header-only, so no new exported symbols. */
static inline const char *ssp4sim_run_state_name(ssp4sim_run_state state)
{
    switch (state)
    {
    case SSP4SIM_STATE_CREATED:
        return "created";
    case SSP4SIM_STATE_INITIALIZED:
        return "initialized";
    case SSP4SIM_STATE_RUNNING:
        return "running";
    case SSP4SIM_STATE_PAUSED:
        return "paused";
    case SSP4SIM_STATE_STOPPING:
        return "stopping";
    case SSP4SIM_STATE_COMPLETED:
        return "completed";
    case SSP4SIM_STATE_STOPPED:
        return "stopped";
    case SSP4SIM_STATE_FAILED:
        return "failed";
    }
    return "failed";
}

static inline const char *ssp4sim_run_result_name(ssp4sim_run_result result)
{
    switch (result)
    {
    case SSP4SIM_RESULT_COMPLETED:
        return "completed";
    case SSP4SIM_RESULT_STOPPED:
        return "stopped";
    case SSP4SIM_RESULT_FAILED:
        return "failed";
    }
    return "failed";
}

ssp4sim_status ssp4sim_simulator_create(
    const char *config_path,
    ssp4sim_simulator_handle **out_handle
);

ssp4sim_status ssp4sim_simulator_init(ssp4sim_simulator_handle *handle);

/* Runs to completion and returns a status only; use wait() for the outcome. */
ssp4sim_status ssp4sim_simulator_simulate(ssp4sim_simulator_handle *handle);

/* Starts the run and blocks until it is Running or terminal. */
ssp4sim_status ssp4sim_simulator_start(ssp4sim_simulator_handle *handle);
ssp4sim_status ssp4sim_simulator_pause(ssp4sim_simulator_handle *handle,
                                       int has_at_sim_time,
                                       uint64_t at_sim_time_ns);
ssp4sim_status ssp4sim_simulator_resume(ssp4sim_simulator_handle *handle);
/* Stops a Running or Paused run. Idempotent once Stopping or terminal; a
 * pre-run stop remains an error. */
ssp4sim_status ssp4sim_simulator_stop(ssp4sim_simulator_handle *handle,
                                      int has_at_sim_time,
                                      uint64_t at_sim_time_ns);
ssp4sim_status ssp4sim_simulator_set_speed(ssp4sim_simulator_handle *handle,
                                           double speed, int has_at_sim_time,
                                           uint64_t at_sim_time_ns);

/* Sole readback: returns one coherent run info record. A null handle returns
 * the zero/default RunInfo (Created, 0, 0) and sets ssp4sim_last_error(). */
ssp4sim_run_info ssp4sim_simulator_get_info(
    ssp4sim_simulator_handle *handle);

/* Blocks until the run finishes. On SSP4SIM_STATUS_OK, *result is a
 * ssp4sim_run_result. A run that failed is reported as an error status with
 * the failure message in ssp4sim_last_error(), never as a completed result. */
ssp4sim_status ssp4sim_simulator_wait(ssp4sim_simulator_handle *handle,
                                      ssp4sim_run_result *result);

void ssp4sim_simulator_destroy(ssp4sim_simulator_handle *handle);

#ifdef __cplusplus
}
#endif
