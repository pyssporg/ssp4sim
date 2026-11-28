#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace ssp4sim::utils
{
    /// Round half away from zero and saturate to int32. NaN maps to 0.
    inline std::int32_t rounded_int32(double value) noexcept
    {
        if (std::isnan(value))
        {
            return 0;
        }
        if (value >=
            static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        {
            return std::numeric_limits<std::int32_t>::max();
        }
        if (value <=
            static_cast<double>(std::numeric_limits<std::int32_t>::min()))
        {
            return std::numeric_limits<std::int32_t>::min();
        }
        return static_cast<std::int32_t>(std::llround(value));
    }
}
