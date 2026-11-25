#pragma once

#include "runtime_control.hpp"

namespace ssp4sim::control
{
    // The public types are canonical; the control layer only aliases them so
    // existing control code can keep its short names.
    using ssp4sim::RunInfo;
    using ssp4sim::RunResult;
    using ssp4sim::RunState;
}
