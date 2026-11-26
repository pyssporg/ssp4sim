#pragma once

#include "c_api/c_api_common.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <string>

namespace ssp4sim::c_api
{
    ssp4sim_status set_error(ssp4sim_status status, const std::string &message);
    void clear_error();

    // The single translation from C++ error types to C status codes.
    template <typename Fn>
    ssp4sim_status translate_exceptions(const char *action, Fn &&fn)
    {
        try
        {
            fn();
            clear_error();
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
}