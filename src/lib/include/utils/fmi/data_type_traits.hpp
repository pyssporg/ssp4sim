#pragma once

#include "ssp4sim_definitions.hpp"

namespace ssp4sim::utils::fmi
{
    /// FMI types whose values can be scaled by an override or used as a
    /// scalar signal: real, integer, boolean.
    inline bool is_scalable(types::DataType type) noexcept
    {
        return type == types::DataType::real ||
               type == types::DataType::integer ||
               type == types::DataType::boolean;
    }

    /// Integer-family types that use the FMU integer read/write calls:
    /// integer and enumeration. Boolean is separate.
    inline bool is_integer_like(types::DataType type) noexcept
    {
        return type == types::DataType::integer ||
               type == types::DataType::enumeration;
    }

    /// Types stored as an in-memory `int`: integer, enumeration and boolean.
    inline bool is_int_backed(types::DataType type) noexcept
    {
        return is_integer_like(type) || type == types::DataType::boolean;
    }

    inline bool is_string(types::DataType type) noexcept
    {
        return type == types::DataType::string;
    }
}
