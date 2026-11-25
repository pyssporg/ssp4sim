#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int ssp4sim_status;

enum
{
    SSP4SIM_STATUS_OK = 0,
    SSP4SIM_STATUS_INVALID_ARGUMENT = 1,
    SSP4SIM_STATUS_RUNTIME_ERROR = 2,
    SSP4SIM_STATUS_OUT_OF_MEMORY = 3
};

/* Thread-local message for the last failing ssp4sim_* call on this thread.
 * Invalidated by the next ssp4sim_* call on the same thread. */
const char *ssp4sim_last_error(void);

#ifdef __cplusplus
}
#endif