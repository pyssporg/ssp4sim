#include <catch.hpp>

#include "utils/fmi/data_type_traits.hpp"
#include "utils/fmi/value_transform.hpp"
#include "utils/primitives/numeric.hpp"

#include <limits>
#include <string>

using ssp4sim::types::DataType;
using ssp4sim::utils::rounded_int32;

namespace fmi = ssp4sim::utils::fmi;

// ---------------------------------------------------------------------------
// Description: Verifies round-half-away-from-zero and int32 saturation
// Rationale:   Override integer semantics (REQ-016) depend on this helper
// ---------------------------------------------------------------------------
TEST_CASE("rounded_int32 rounds half away from zero", "[numeric]")
{
    REQUIRE(rounded_int32(0.0) == 0);
    REQUIRE(rounded_int32(1.4) == 1);
    REQUIRE(rounded_int32(1.5) == 2);
    REQUIRE(rounded_int32(2.5) == 3);
    REQUIRE(rounded_int32(-1.4) == -1);
    REQUIRE(rounded_int32(-1.5) == -2);
    REQUIRE(rounded_int32(-2.5) == -3);
}

TEST_CASE("rounded_int32 saturates to int32", "[numeric]")
{
    REQUIRE(rounded_int32(
                static_cast<double>(std::numeric_limits<std::int32_t>::max()) +
                1.0) == std::numeric_limits<std::int32_t>::max());
    REQUIRE(rounded_int32(
                static_cast<double>(std::numeric_limits<std::int32_t>::min()) -
                1.0) == std::numeric_limits<std::int32_t>::min());
    REQUIRE(rounded_int32(std::numeric_limits<double>::infinity()) ==
            std::numeric_limits<std::int32_t>::max());
    REQUIRE(rounded_int32(-std::numeric_limits<double>::infinity()) ==
            std::numeric_limits<std::int32_t>::min());
    REQUIRE(rounded_int32(std::numeric_limits<double>::quiet_NaN()) == 0);
}

TEST_CASE("FMI data type classification", "[numeric]")
{
    REQUIRE(fmi::is_scalable(DataType::real));
    REQUIRE(fmi::is_scalable(DataType::integer));
    REQUIRE(fmi::is_scalable(DataType::boolean));
    REQUIRE_FALSE(fmi::is_scalable(DataType::string));
    REQUIRE_FALSE(fmi::is_scalable(DataType::enumeration));

    REQUIRE(fmi::is_integer_like(DataType::integer));
    REQUIRE(fmi::is_integer_like(DataType::enumeration));
    REQUIRE_FALSE(fmi::is_integer_like(DataType::boolean));

    REQUIRE(fmi::is_int_backed(DataType::integer));
    REQUIRE(fmi::is_int_backed(DataType::enumeration));
    REQUIRE(fmi::is_int_backed(DataType::boolean));
    REQUIRE_FALSE(fmi::is_int_backed(DataType::real));

    REQUIRE(fmi::is_string(DataType::string));
    REQUIRE_FALSE(fmi::is_string(DataType::real));
}

TEST_CASE("apply_affine scales real, integer and boolean values", "[numeric]")
{
    double real = 2.0;
    fmi::apply_affine(DataType::real, &real, 3.0, 1.0);
    REQUIRE(real == 7.0);

    int integer = 1;
    fmi::apply_affine(DataType::integer, &integer, 2.5, 0.0);
    REQUIRE(integer == 3);

    int boolean = 2;
    fmi::apply_affine(DataType::boolean, &boolean, 0.0, 0.0);
    REQUIRE(boolean == 0);

    boolean = 0;
    fmi::apply_affine(DataType::boolean, &boolean, 0.0, -1.0);
    REQUIRE(boolean == 1);

    std::string text = "unchanged";
    fmi::apply_affine(DataType::string, &text, 2.0, 1.0);
    REQUIRE(text == "unchanged");
}
