#include "c_api_common.h"

#include "c_api/c_api_internal.hpp"

#include <string>

namespace
{
    thread_local std::string g_last_error;
}

namespace ssp4sim::c_api
{
    ssp4sim_status set_error(const ssp4sim_status status,
                             const std::string &message)
    {
        g_last_error = message;
        return status;
    }

    void clear_error()
    {
        g_last_error.clear();
    }
}

const char *ssp4sim_last_error(void)
{
    return g_last_error.c_str();
}