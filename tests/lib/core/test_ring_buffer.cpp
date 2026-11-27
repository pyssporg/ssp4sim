#include "utils/primitives/ring_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <stdexcept>

using ssp4sim::utils::RingBuffer;

// ---------------------------------------------------------------------------
// Description: Verifies RingBuffer rejects construction with zero capacity
// Rationale:   Zero-capacity buffers cause division-by-zero in push/find logic
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer rejects zero capacity", "[RingBuffer]")
{
    REQUIRE_THROWS_AS(RingBuffer(0, sizeof(std::uint32_t)), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Description: Verifies push grows until full then overwrites oldest entries
// Rationale:   Core ring-buffer contract — wraparound and overwrite semantics
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer push grows until full then overwrites oldest", "[RingBuffer]")
{
    constexpr std::size_t kCapacity = 3;
    RingBuffer buffer(kCapacity, sizeof(std::uint32_t));

    REQUIRE(buffer.is_empty());
    REQUIRE_FALSE(buffer.is_full());
    REQUIRE(buffer.head == 0);

    // first
    auto expected_head = 1;
    auto first_index = buffer.push();
    REQUIRE(first_index == expected_head);
    REQUIRE(buffer.head == expected_head);
    REQUIRE(buffer.write_count == 1);
    REQUIRE_FALSE(buffer.is_empty());

    // second
    expected_head = 2;
    auto second_index = buffer.push();
    REQUIRE(second_index == expected_head);
    REQUIRE(buffer.head == expected_head);
    REQUIRE(buffer.write_count == 2);

    // 3 wraparound
    expected_head = 0;
    auto third_index = buffer.push();
    REQUIRE(third_index == expected_head);
    REQUIRE(buffer.head == expected_head);
    REQUIRE(buffer.write_count == 3);
    REQUIRE(buffer.is_full());

    // 4 overwrite 
    expected_head = 1;
    auto fourth_index = buffer.push();
    REQUIRE(fourth_index == expected_head);
    REQUIRE(buffer.write_count == 4);
}

// ---------------------------------------------------------------------------
// Description: Verifies get_item provides aligned storage and bounds-checking
// Rationale:   Memory safety — misaligned access causes faults on ARM
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer get_item provides aligned storage and bounds checks", "[RingBuffer]")
{
    constexpr std::size_t element_size = sizeof(std::uint64_t);
    RingBuffer buffer(3, element_size);

    auto idx1 = buffer.push();
    // should 
    buffer.get_item(idx1);
    REQUIRE_THROWS_AS(buffer.get_item(idx1+1), std::runtime_error); // ensure that its not out of bounds

    auto idx2 = buffer.push();

    auto *first = buffer.get_item(idx1);
    auto *second = buffer.get_item(idx2);
    const auto first_address = reinterpret_cast<std::uintptr_t>(first);
    const auto second_address = reinterpret_cast<std::uintptr_t>(second);
    const auto expected_stride = ssp4sim::utils::align_up(element_size, ssp4sim::utils::target_alignment);

    REQUIRE(buffer.buffers.aligned_size == expected_stride);
    REQUIRE(first_address % ssp4sim::utils::target_alignment == 0);
    REQUIRE(second_address % ssp4sim::utils::target_alignment == 0);
    REQUIRE(second - first == static_cast<std::ptrdiff_t>(expected_stride));
    REQUIRE(second - first >= static_cast<std::ptrdiff_t>(element_size));

    *reinterpret_cast<std::uint64_t *>(first) = 42;
    *reinterpret_cast<std::uint64_t *>(second) = 99;
    REQUIRE(*reinterpret_cast<std::uint64_t *>(buffer.get_item(idx1)) == 42);
    REQUIRE(*reinterpret_cast<std::uint64_t *>(buffer.get_item(idx2)) == 99);

    auto idx3 = buffer.push();

    // access outside bounds when full
    REQUIRE_THROWS_AS(buffer.get_item(buffer.capacity), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Description: Verifies index_back_from_head follows head/tail ordering
//              before and after wraparound
// Rationale:   Reverse-position lookup is used by signal storage for
//              Nth-most-recent-value queries
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer index helpers follow head/tail ordering", "[RingBuffer]")
{
    RingBuffer buffer(3, sizeof(int));
    

    auto idx1 = buffer.push();
    *reinterpret_cast<int *>(buffer.get_item(idx1)) = 10;
    // ensure this works while buffer is not full
    auto index = buffer.index_back_from_head(0);
    REQUIRE(index == idx1);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(index)) == 10);
    
    auto idx2 = buffer.push();
    *reinterpret_cast<int *>(buffer.get_item(idx2)) = 20;
    index = buffer.index_back_from_head(1);
    REQUIRE(index == idx1);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(index)) == 10);

    auto idx3 = buffer.push();
    *reinterpret_cast<int *>(buffer.get_item(idx3)) = 30;
    REQUIRE(buffer.is_full());

    // Reverse lookup should walk newest to oldest.
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(0))) == 30);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(1))) == 20);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(2))) == 10);

    // Overwrite oldest and confirm head/tail advance and value mapping update.
    auto idx4 = buffer.push();
    *reinterpret_cast<int *>(buffer.get_item(idx4)) = 40;

    REQUIRE(buffer.is_full());

    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(0))) == 40);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(1))) == 30);
    REQUIRE(*reinterpret_cast<int *>(buffer.get_item(buffer.index_back_from_head(2))) == 20);
}

// ---------------------------------------------------------------------------
// Description: Verifies timestamp-index mapping through wraparound/overwrite
// Rationale:   Timestamp lookup is the primary access pattern for signal routing
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer tracks timestamps and finds valid indices", "[RingBuffer]")
{
    RingBuffer buffer(3, sizeof(int));

    buffer.push(100);
    REQUIRE(buffer.get_time(buffer.head) == 100);

    auto idx_200 = buffer.push(200);
    REQUIRE(buffer.get_time(buffer.head) == 200);
    // ensure these work while buffer is not full
    size_t index_for_200;
    REQUIRE(buffer.find_exact_index(200, index_for_200) == true);
    REQUIRE(index_for_200 == idx_200);
    size_t index;
    REQUIRE(buffer.find_latest_valid_index(250, index) == true);
    REQUIRE(index == idx_200);

    auto idx3 = buffer.push(300);
    REQUIRE(buffer.get_time(buffer.head) == 300);

    REQUIRE(buffer.find_exact_index(200, index_for_200) == true);
    REQUIRE(index_for_200 == idx_200);
    REQUIRE(buffer.get_time(index_for_200) == 200);

    REQUIRE(buffer.find_latest_valid_index(250, index) == true);
    REQUIRE(index == index_for_200);
    REQUIRE(buffer.find_latest_valid_index(50, index) == false);

    buffer.push(400); // overwrites oldest timestamp
    REQUIRE(buffer.find_exact_index(100, index) == false);
    REQUIRE(buffer.find_latest_valid_index(100, index) == false);
    REQUIRE(buffer.find_latest_valid_index(150, index) == false);
    REQUIRE(buffer.find_latest_valid_index(350, index) == true);
    REQUIRE(index == idx3);

    buffer.push(500); // overwrites oldest timestamp

    REQUIRE(buffer.find_latest_valid_index(100, index) == false);
    REQUIRE(buffer.find_latest_valid_index(299, index) == false);
    REQUIRE(buffer.find_latest_valid_index(300, index) == true);
    REQUIRE(buffer.find_latest_valid_index(301, index) == true);
}

// ---------------------------------------------------------------------------
// Description: Verifies serialization captures all metadata and payload bytes
//              and deserialization reproduces them without reallocating
// Rationale:   The wire round-trip must be lossless for mid-run state capture
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer serialization round-trips metadata and payload", "[RingBuffer]")
{
    constexpr std::size_t capacity = 4;
    RingBuffer buffer(capacity, sizeof(std::uint32_t));

    const auto first = buffer.push(10);
    *reinterpret_cast<std::uint32_t *>(buffer.get_item(first)) = 111;
    const auto second = buffer.push(20);
    *reinterpret_cast<std::uint32_t *>(buffer.get_item(second)) = 222;

    ssp4sim::serialize::Writer writer;
    buffer.serialize(writer);
    const std::vector<std::byte> bytes = writer.take();

    RingBuffer restored(capacity, sizeof(std::uint32_t));
    ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
    restored.deserialize(reader);
    reader.expect_end();

    REQUIRE(restored.buffers.original_size == buffer.buffers.original_size);
    REQUIRE(restored.buffers.aligned_size == buffer.buffers.aligned_size);
    REQUIRE(restored.capacity == buffer.capacity);
    REQUIRE(restored.head == buffer.head);
    REQUIRE(restored.write_count == buffer.write_count);
    REQUIRE(restored.timestamps == buffer.timestamps);
    REQUIRE(restored.populated == buffer.populated);
    REQUIRE(restored.get_time(first) == 10);
    REQUIRE(restored.get_time(second) == 20);
    REQUIRE(*reinterpret_cast<std::uint32_t *>(restored.get_item(first)) == 111);
    REQUIRE(*reinterpret_cast<std::uint32_t *>(restored.get_item(second)) == 222);
}

// ---------------------------------------------------------------------------
// Description: Verifies pushes after a serialization are undone by
//              deserialization, making older values and timestamps visible again
// Rationale:   Proves deserialization actually rewinds the logical ring
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer deserialization rewinds after later pushes", "[RingBuffer]")
{
    constexpr std::size_t capacity = 3;
    RingBuffer buffer(capacity, sizeof(std::uint32_t));

    const auto older = buffer.push(100);
    *reinterpret_cast<std::uint32_t *>(buffer.get_item(older)) = 1;
    const auto newer = buffer.push(200);
    *reinterpret_cast<std::uint32_t *>(buffer.get_item(newer)) = 2;

    ssp4sim::serialize::Writer writer;
    buffer.serialize(writer);
    const std::vector<std::byte> bytes = writer.take();

    // Two further pushes wrap the ring and overwrite the older slot.
    buffer.push(300);
    const auto overwriter = buffer.push(400);
    *reinterpret_cast<std::uint32_t *>(buffer.get_item(overwriter)) = 4;
    std::size_t found = 0;
    REQUIRE(buffer.find_exact_index(100, found) == false);

    ssp4sim::serialize::Reader reader{std::span<const std::byte>(bytes)};
    buffer.deserialize(reader);
    reader.expect_end();

    REQUIRE(buffer.write_count == 2);
    REQUIRE_FALSE(buffer.is_full());
    REQUIRE(buffer.find_exact_index(100, found) == true);
    REQUIRE(found == older);
    REQUIRE(buffer.find_exact_index(200, found) == true);
    REQUIRE(found == newer);
    REQUIRE(buffer.find_exact_index(300, found) == false);
    REQUIRE(buffer.find_exact_index(400, found) == false);
    REQUIRE(*reinterpret_cast<std::uint32_t *>(buffer.get_item(older)) == 1);
    REQUIRE(*reinterpret_cast<std::uint32_t *>(buffer.get_item(newer)) == 2);
}

// ---------------------------------------------------------------------------
// Description: Verifies deserialization rejects state from an incompatible ring
// Rationale:   Identity mismatches would copy the wrong layout into the buffer
// ---------------------------------------------------------------------------
TEST_CASE("RingBuffer deserialization rejects identity mismatch", "[RingBuffer]")
{
    RingBuffer source(3, sizeof(std::uint32_t));
    source.push(10);
    ssp4sim::serialize::Writer writer;
    source.serialize(writer);
    const std::vector<std::byte> bytes = writer.take();

    RingBuffer wider(4, sizeof(std::uint32_t));
    ssp4sim::serialize::Reader wider_reader{std::span<const std::byte>(bytes)};
    REQUIRE_THROWS_AS(wider.deserialize(wider_reader), ssp4sim::serialize::Error);

    RingBuffer larger_element(3, sizeof(std::uint64_t));
    ssp4sim::serialize::Reader element_reader{std::span<const std::byte>(bytes)};
    REQUIRE_THROWS_AS(larger_element.deserialize(element_reader),
                      ssp4sim::serialize::Error);
}
