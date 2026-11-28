#pragma once

#include "ssp4sim_definitions.hpp"
#include "utils/fmi/data_type_traits.hpp"
#include "utils/primitives/numeric.hpp"

namespace ssp4sim::utils::fmi
{
    /// Apply y = k*x + m in place to one scalar value. Real values use
    /// floating point; integer and enumeration round half away from zero and
    /// saturate to int32; boolean becomes 1 when the result is nonzero.
    /// String and unknown values are left untouched.
    inline void apply_affine(types::DataType type, void *value,
                             double k, double m) noexcept
    {
        if (type == types::DataType::real)
        {
            auto *item = static_cast<double *>(value);
            *item = k * *item + m;
            return;
        }
        if (is_integer_like(type))
        {
            auto *item = static_cast<int *>(value);
            *item = utils::rounded_int32(k * static_cast<double>(*item) + m);
            return;
        }
        if (type == types::DataType::boolean)
        {
            auto *item = static_cast<int *>(value);
            *item = (k * static_cast<double>(*item) + m) != 0.0 ? 1 : 0;
        }
    }
}
