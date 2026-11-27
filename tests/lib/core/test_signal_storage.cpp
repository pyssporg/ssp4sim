#include "simulation/signal/storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using ssp4sim::signal::SignalStorage;
using ssp4sim::types::DataType;

namespace
{
    std::vector<std::byte> serialize_storage(const SignalStorage &storage)
    {
        ssp4sim::serialize::Writer writer;
        storage.serialize(writer);
        return writer.take();
    }

    void deserialize_storage(const std::vector<std::byte> &bytes,
                             SignalStorage &storage)
    {
        ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
        storage.deserialize(reader);
        reader.expect_end();
    }

    struct ParsedStorageSection
    {
        std::string name;
        std::size_t areas = 0;
        std::size_t area_byte_size = 0;
        std::size_t ring_capacity = 0;
        std::size_t original_size = 0;
        std::size_t aligned_size = 0;
        std::size_t capacity = 0;
        std::size_t head = 0;
        std::size_t write_count = 0;
        std::vector<std::uint64_t> timestamps;
        std::vector<std::uint8_t> populated;
        std::span<const std::byte> data;
    };

    ParsedStorageSection parse_storage_section(
        const std::vector<std::byte> &bytes)
    {
        ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
        ParsedStorageSection parsed;

        parsed.name = reader.read_string();
        parsed.areas = static_cast<std::size_t>(reader.read_u64());
        parsed.area_byte_size = static_cast<std::size_t>(reader.read_u64());
        parsed.ring_capacity = static_cast<std::size_t>(reader.read_u64());

        parsed.original_size = static_cast<std::size_t>(reader.read_u64());
        parsed.aligned_size = static_cast<std::size_t>(reader.read_u64());
        parsed.capacity = static_cast<std::size_t>(reader.read_u64());
        parsed.head = static_cast<std::size_t>(reader.read_u64());
        parsed.write_count = static_cast<std::size_t>(reader.read_u64());

        const std::uint64_t timestamp_count = reader.read_u64();
        parsed.timestamps.reserve(static_cast<std::size_t>(timestamp_count));
        for (std::uint64_t i = 0; i < timestamp_count; ++i)
        {
            parsed.timestamps.push_back(reader.read_u64());
        }

        const std::uint64_t populated_count = reader.read_u64();
        parsed.populated.reserve(static_cast<std::size_t>(populated_count));
        for (std::uint64_t i = 0; i < populated_count; ++i)
        {
            parsed.populated.push_back(reader.read_u8());
        }

        parsed.data = reader.read_blob();
        return parsed;
    }
}

// ---------------------------------------------------------------------------
// Description: Verifies aligned layout, correct offsets, area_byte_size stride,
//              and derivative pointer arithmetic
// Rationale:   Misaligned access causes crashes; wrong offsets corrupt data
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage allocates variable and derivative layout", "[SignalStorage]")
{
    SignalStorage storage(3, "signals");
    const auto real_index = storage.add_variable("signals.real", DataType::real, 2);
    const auto int_index = storage.add_variable("signals.mode", DataType::integer, 0);

    storage.allocate();

    auto area0 = storage.push(100);
    auto *area0_real = storage.get_item(area0, real_index);
    auto *area0_int = storage.get_item(area0, int_index);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area0_real) % alignof(double) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area0_int) % alignof(int) == 0);
    REQUIRE(reinterpret_cast<std::byte *>(area0_int) - reinterpret_cast<std::byte *>(area0_real) ==
            static_cast<std::ptrdiff_t>(storage.variables[int_index].position - storage.variables[real_index].position));

    auto area1 = storage.push(200);
    auto *area1_real = storage.get_item(area1, real_index);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area1_real) % alignof(double) == 0);
    REQUIRE(reinterpret_cast<std::byte *>(area1_real) - reinterpret_cast<std::byte *>(area0_real) == static_cast<std::ptrdiff_t>(storage.area_byte_size));

    auto *first_derivative = storage.get_derivative(area0, real_index, 1);
    auto *second_derivative = storage.get_derivative(area0, real_index, 2);
    REQUIRE(first_derivative != nullptr);
    REQUIRE(second_derivative != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(first_derivative) % alignof(double) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(second_derivative) % alignof(double) == 0);
    REQUIRE(second_derivative - first_derivative == static_cast<std::ptrdiff_t>(sizeof(double)));
}

// ---------------------------------------------------------------------------
// Description: Verifies alignment for mixed types (int, double, string)
// Rationale:   Mixed-type alignment is the most error-prone layout scenario
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage aligns mixed-type values and derivatives", "[SignalStorage]")
{
    SignalStorage storage(2, "signals");
    const auto int_index = storage.add_variable("signals.mode", DataType::integer, 0);
    const auto real_index = storage.add_variable("signals.real", DataType::real, 1);
    const auto string_index = storage.add_variable("signals.label", DataType::string, 0);
    storage.allocate();

    REQUIRE(storage.variables[int_index].position % alignof(int) == 0);
    REQUIRE(storage.variables[real_index].position % alignof(double) == 0);
    REQUIRE(storage.variables[real_index].derivative_position % alignof(double) == 0);
    REQUIRE(storage.variables[string_index].position % alignof(std::string) == 0);
    REQUIRE(storage.area_byte_size % alignof(std::max_align_t) == 0);

    const auto area0 = storage.push(10);
    const auto area1 = storage.push(20);

    auto *area0_real = storage.get_item(area0, real_index);
    auto *area1_real = storage.get_item(area1, real_index);
    auto *area0_string = storage.get_item(area0, string_index);
    auto *area1_string = storage.get_item(area1, string_index);

    REQUIRE(reinterpret_cast<std::uintptr_t>(area0_real) % alignof(double) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area1_real) % alignof(double) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area0_string) % alignof(std::string) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(area1_string) % alignof(std::string) == 0);
}

// ---------------------------------------------------------------------------
// Description: Verifies push/get_time/find_area/find_latest_valid_area
//              through wraparound
// Rationale:   Timestamp-based lookup is the primary access pattern
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage pushes timestamps and finds areas", "[SignalStorage]")
{
    SignalStorage storage(3, "signals");
    storage.add_variable("signals.temperature", DataType::real, 1);
    storage.allocate();

    auto first = storage.push(100);
    auto second = storage.push(200);
    auto third = storage.push(300);

    REQUIRE(storage.get_time(first) == 100);
    REQUIRE(storage.get_time(second) == 200);
    REQUIRE(storage.get_time(third) == 300);

    size_t index_found;
    REQUIRE(storage.find_area(200, index_found) == true);
    REQUIRE(index_found == second);

    REQUIRE(storage.find_latest_valid_area(250, index_found) == true);
    REQUIRE(index_found == second);
    REQUIRE(storage.find_latest_valid_area(50, index_found) == false);

    auto fourth = storage.push(400); // overwrite oldest
    REQUIRE(storage.get_time(fourth) == 400);
    REQUIRE(storage.find_area(100, index_found) == false);

    REQUIRE(storage.find_latest_valid_area(350, index_found) == true);
    REQUIRE(index_found == third);
}

// ---------------------------------------------------------------------------
// Description: Verifies null returns for out-of-range derivative requests
// Rationale:   Safety — prevents null-pointer dereference
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage returns null for derivative requests outside valid range", "[SignalStorage]")
{
    SignalStorage storage(2, "signals");
    const auto no_derivative_index = storage.add_variable("signals.mode", DataType::integer, 0);
    const auto derivative_index = storage.add_variable("signals.real", DataType::real, 2);

    REQUIRE(storage.get_item(0, 0) == nullptr);
    REQUIRE(storage.get_derivative(0, derivative_index, 1) == nullptr);

    storage.allocate();
    const auto area = storage.push(100);

    REQUIRE(storage.get_derivative(area, no_derivative_index, 1) == nullptr);
    REQUIRE(storage.get_derivative(area, derivative_index, 0) == nullptr);
    REQUIRE(storage.get_derivative(area, derivative_index, 1) != nullptr);
    REQUIRE(storage.get_derivative(area, derivative_index, 2) != nullptr);
    REQUIRE(storage.get_derivative(area, derivative_index, 3) == nullptr);
}

// ---------------------------------------------------------------------------
// Description: Verifies double-allocate throws std::runtime_error
// Rationale:   Double allocation corrupts memory layout
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage allocate can only be called once", "[SignalStorage]")
{
    SignalStorage storage(2, "signals");
    storage.add_variable("signals.value", DataType::integer, 0);

    REQUIRE_NOTHROW(storage.allocate());
    REQUIRE_THROWS_AS(storage.allocate(), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Description: Verifies get_or_push reuses areas for the same timestamp
// Rationale:   Coalescing prevents duplicate allocations per timestep
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage get_or_push reuses existing timestamp areas", "[SignalStorage]")
{
    SignalStorage storage(3, "signals");
    storage.add_variable("signals.temperature", DataType::real, 0);
    storage.allocate();

    const auto first = storage.get_or_push(100);
    const auto second = storage.get_or_push(100);
    const auto third = storage.get_or_push(200);

    REQUIRE(first == second);
    REQUIRE(first != third);

    size_t found = 0;
    REQUIRE(storage.find_area(100, found));
    REQUIRE(found == first);
    REQUIRE(storage.find_area(200, found));
    REQUIRE(found == third);
}

// ---------------------------------------------------------------------------
// Description: Verifies serialization captures identity and ring payload and
//              deserialization reproduces it
// Rationale:   The wire round-trip must be lossless for mid-run state capture
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage serialization round-trips ring state", "[SignalStorage]")
{
    SignalStorage storage(3, "signals");
    const auto value_index = storage.add_variable("signals.temperature", DataType::real, 1);
    storage.allocate();

    const auto first = storage.push(100);
    const auto second = storage.push(200);
    *reinterpret_cast<double *>(storage.get_item(first, value_index)) = 1.5;
    *reinterpret_cast<double *>(storage.get_item(second, value_index)) = 2.5;
    *reinterpret_cast<double *>(storage.get_derivative(first, value_index, 1)) = 0.5;

    const std::vector<std::byte> bytes = serialize_storage(storage);
    const ParsedStorageSection parsed = parse_storage_section(bytes);

    REQUIRE(parsed.name == storage.name);
    REQUIRE(parsed.areas == storage.areas);
    REQUIRE(parsed.area_byte_size == storage.area_byte_size);
    REQUIRE(parsed.ring_capacity == storage.ring->capacity);
    REQUIRE(parsed.capacity == storage.ring->capacity);
    REQUIRE(parsed.data.size() == storage.ring->buffers.total_size);

    SignalStorage restored(3, "signals");
    restored.add_variable("signals.temperature", DataType::real, 1);
    restored.allocate();
    deserialize_storage(bytes, restored);

    REQUIRE(restored.get_time(first) == 100);
    REQUIRE(restored.get_time(second) == 200);
    REQUIRE(*reinterpret_cast<double *>(restored.get_item(first, value_index)) == 1.5);
    REQUIRE(*reinterpret_cast<double *>(restored.get_item(second, value_index)) == 2.5);
    REQUIRE(*reinterpret_cast<double *>(
                restored.get_derivative(first, value_index, 1)) == 0.5);
}

// ---------------------------------------------------------------------------
// Description: Verifies pushes after serialization are undone by
//              deserialization so the older values and timestamps become
//              visible again
// Rationale:   Proves deserialization rewinds the logical areas, not just bytes
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage deserialization rewinds areas after later pushes", "[SignalStorage]")
{
    SignalStorage storage(3, "signals");
    const auto value_index = storage.add_variable("signals.temperature", DataType::real, 0);
    storage.allocate();

    const auto older = storage.push(100);
    const auto newer = storage.push(200);
    *reinterpret_cast<double *>(storage.get_item(older, value_index)) = 10.0;
    *reinterpret_cast<double *>(storage.get_item(newer, value_index)) = 20.0;

    const std::vector<std::byte> bytes = serialize_storage(storage);

    // Wrap the ring; the fourth area overwrites the older slot.
    storage.push(300);
    const auto overwriter = storage.push(400);
    *reinterpret_cast<double *>(storage.get_item(overwriter, value_index)) = 40.0;

    std::size_t found = 0;
    REQUIRE(storage.find_area(100, found) == false);
    REQUIRE(storage.find_area(300, found) == true);

    deserialize_storage(bytes, storage);

    REQUIRE(storage.get_time(older) == 100);
    REQUIRE(storage.get_time(newer) == 200);
    REQUIRE(*reinterpret_cast<double *>(storage.get_item(older, value_index)) == 10.0);
    REQUIRE(*reinterpret_cast<double *>(storage.get_item(newer, value_index)) == 20.0);
    REQUIRE(storage.find_area(100, found) == true);
    REQUIRE(found == older);
    REQUIRE(storage.find_area(300, found) == false);
    REQUIRE(storage.find_area(400, found) == false);
}

// ---------------------------------------------------------------------------
// Description: Verifies deserialization rejects state from an incompatible
//              storage
// Rationale:   Identity mismatches would copy the wrong layout into the ring
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage deserialization rejects identity mismatch", "[SignalStorage]")
{
    SignalStorage source(2, "source");
    source.add_variable("source.value", DataType::real, 0);
    source.allocate();
    const std::vector<std::byte> bytes = serialize_storage(source);

    SignalStorage wrong_name(2, "other");
    wrong_name.add_variable("other.value", DataType::real, 0);
    wrong_name.allocate();
    {
        ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
        REQUIRE_THROWS_AS(wrong_name.deserialize(reader), ssp4sim::serialize::Error);
    }

    SignalStorage wrong_areas(3, "source");
    wrong_areas.add_variable("source.value", DataType::real, 0);
    wrong_areas.allocate();
    {
        ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
        REQUIRE_THROWS_AS(wrong_areas.deserialize(reader), ssp4sim::serialize::Error);
    }

    SignalStorage wrong_layout(2, "source");
    wrong_layout.add_variable("source.value", DataType::real, 0);
    wrong_layout.add_variable("source.extra", DataType::real, 0);
    wrong_layout.allocate();
    {
        ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
        REQUIRE_THROWS_AS(wrong_layout.deserialize(reader), ssp4sim::serialize::Error);
    }
}

// ---------------------------------------------------------------------------
// Description: Verifies string values are serialized as typed values while
//              their raw ring bytes are zeroed, then restored in place
// Rationale:   Raw copying an in-place std::string would save process-local
//              pointers; typed serialization is safe across processes
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage serialization round-trips string values", "[SignalStorage]")
{
    SignalStorage storage(3, "strings");
    const auto real_index = storage.add_variable("strings.real", DataType::real, 0);
    const auto string_index = storage.add_variable("strings.label", DataType::string, 0);
    storage.allocate();

    const auto first = storage.push(100);
    const auto second = storage.push(200);
    const auto third = storage.push(300);

    const std::string long_value(64, 'x');
    const std::vector<std::string> expected_strings = {"hi", long_value, "ok"};
    const std::vector<double> expected_reals = {1.25, 2.5, 3.75};
    const std::vector<std::size_t> areas = {first, second, third};

    std::vector<std::string> expected_strings_by_area(storage.areas);
    std::vector<double> expected_reals_by_area(storage.areas);
    for (std::size_t a = 0; a < areas.size(); ++a)
    {
        *reinterpret_cast<std::string *>(storage.get_item(areas[a], string_index)) =
            expected_strings[a];
        *reinterpret_cast<double *>(storage.get_item(areas[a], real_index)) =
            expected_reals[a];
        expected_strings_by_area[areas[a]] = expected_strings[a];
        expected_reals_by_area[areas[a]] = expected_reals[a];
    }

    const std::vector<std::byte> bytes = serialize_storage(storage);
    const ParsedStorageSection parsed = parse_storage_section(bytes);

    REQUIRE(parsed.name == "strings");
    REQUIRE(parsed.areas == storage.areas);
    REQUIRE(parsed.area_byte_size == storage.area_byte_size);
    REQUIRE(parsed.ring_capacity == storage.ring->capacity);
    REQUIRE(parsed.data.size() == storage.ring->buffers.total_size);

    const std::size_t string_position = storage.variables[string_index].position;
    const std::size_t string_size = storage.variables[string_index].type_size;
    const std::size_t real_position = storage.variables[real_index].position;
    for (std::size_t a = 0; a < areas.size(); ++a)
    {
        const std::size_t string_offset =
            a * parsed.aligned_size + string_position;
        for (std::size_t b = 0; b < string_size; ++b)
        {
            REQUIRE(parsed.data[string_offset + b] == std::byte{0});
        }

        double saved_real = 0.0;
        std::memcpy(&saved_real,
                    parsed.data.data() + a * parsed.aligned_size + real_position,
                    sizeof(double));
        REQUIRE(saved_real == expected_reals_by_area[a]);
    }

    SignalStorage restored(3, "strings");
    restored.add_variable("strings.real", DataType::real, 0);
    restored.add_variable("strings.label", DataType::string, 0);
    restored.allocate();
    deserialize_storage(bytes, restored);

    REQUIRE(restored.get_time(first) == 100);
    REQUIRE(restored.get_time(second) == 200);
    REQUIRE(restored.get_time(third) == 300);
    for (std::size_t a = 0; a < areas.size(); ++a)
    {
        REQUIRE(*reinterpret_cast<std::string *>(
                    restored.get_item(areas[a], string_index)) == expected_strings[a]);
        REQUIRE(*reinterpret_cast<double *>(
                    restored.get_item(areas[a], real_index)) == expected_reals[a]);
    }
}

// ---------------------------------------------------------------------------
// Description: Verifies string validation happens before any live string is
//              destroyed, so rejected state leaves the live values intact
// Rationale:   A failed deserialization must not lose the current run state
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage deserialization validates strings before mutation", "[SignalStorage]")
{
    SignalStorage storage(2, "validated");
    const auto string_index = storage.add_variable("validated.label", DataType::string, 0);
    storage.allocate();

    const auto first = storage.push(100);
    const auto second = storage.push(200);
    *reinterpret_cast<std::string *>(storage.get_item(first, string_index)) = "first";
    *reinterpret_cast<std::string *>(storage.get_item(second, string_index)) = "second";

    // Same identity and layout, different variable name: the string entry name
    // check must reject this before the live strings are reconstructed.
    SignalStorage corrupt(2, "validated");
    corrupt.add_variable("validated.other", DataType::string, 0);
    corrupt.allocate();
    corrupt.push(100);
    corrupt.push(200);
    const std::vector<std::byte> bytes = serialize_storage(corrupt);

    ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
    REQUIRE_THROWS_AS(storage.deserialize(reader), ssp4sim::serialize::Error);
    REQUIRE(*reinterpret_cast<std::string *>(storage.get_item(first, string_index)) ==
            "first");
    REQUIRE(*reinterpret_cast<std::string *>(storage.get_item(second, string_index)) ==
            "second");
}

// ---------------------------------------------------------------------------
// Description: Verifies string variables are detectable
// Rationale:   Typed string serialization and diagnostics key off this helper
// ---------------------------------------------------------------------------
TEST_CASE("SignalStorage reports string variables", "[SignalStorage]")
{
    SignalStorage numeric(2, "numeric");
    numeric.add_variable("numeric.real", DataType::real, 0);
    numeric.add_variable("numeric.mode", DataType::integer, 0);
    REQUIRE_FALSE(numeric.has_string_variables());

    SignalStorage with_string(2, "with_string");
    with_string.add_variable("with_string.real", DataType::real, 0);
    with_string.add_variable("with_string.label", DataType::string, 0);
    REQUIRE(with_string.has_string_variables());
}
