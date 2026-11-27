#include "simulation/serialize/serialize.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using ssp4sim::serialize::ByteRange;
using ssp4sim::serialize::Error;
using ssp4sim::serialize::Reader;
using ssp4sim::serialize::Unsupported;
using ssp4sim::serialize::Writer;

namespace
{
    std::string_view utf8(const char8_t *text)
    {
        return std::string_view(reinterpret_cast<const char *>(text));
    }
}

// ---------------------------------------------------------------------------
// Description: Round-trips every Writer primitive through a Reader
// Rationale:   The serialization contract is little-endian and lossless
// ---------------------------------------------------------------------------
TEST_CASE("Writer and Reader round trip every primitive", "[serialize]")
{
    const std::array<std::byte, 3> raw = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x80}};

    Writer writer;
    writer.write_u8(0xA5);
    writer.write_u32(0xDEADBEEF);
    writer.write_u64(0x0123456789ABCDEFULL);
    writer.write_string("hello");
    writer.write_bytes(raw);
    writer.write_blob(raw);

    const auto data = writer.take();
    Reader reader(data);

    REQUIRE(reader.read_u8() == 0xA5);
    REQUIRE(reader.read_u32() == 0xDEADBEEF);
    REQUIRE(reader.read_u64() == 0x0123456789ABCDEFULL);
    REQUIRE(reader.read_string() == "hello");

    const auto bytes = reader.read_bytes(raw.size());
    REQUIRE(std::equal(bytes.begin(), bytes.end(), raw.begin(), raw.end()));

    const auto blob = reader.read_blob();
    REQUIRE(std::equal(blob.begin(), blob.end(), raw.begin(), raw.end()));

    reader.expect_end();
}

// ---------------------------------------------------------------------------
// Description: Truncated input throws serialize::Error instead of overrunning
// Rationale:   Bounds-checked reads are the Reader's core safety contract
// ---------------------------------------------------------------------------
TEST_CASE("Reader rejects truncated input", "[serialize]")
{
    Writer writer;
    writer.write_u32(0xDEADBEEF);
    const auto data = writer.take();

    Reader reader(std::span<const std::byte>(data.data(), data.size() - 1));
    REQUIRE_THROWS_AS(reader.read_u32(), Error);

    Writer string_writer;
    string_writer.write_string("abc");
    const auto string_data = string_writer.take();

    Reader string_reader(
        std::span<const std::byte>(string_data.data(), string_data.size() - 1));
    REQUIRE_THROWS_AS(string_reader.read_string(), Error);
}

// ---------------------------------------------------------------------------
// Description: expect_end rejects trailing bytes after a complete section
// Rationale:   A section must consume its input exactly; leftovers signal a bug
// ---------------------------------------------------------------------------
TEST_CASE("Reader rejects trailing bytes", "[serialize]")
{
    Writer writer;
    writer.write_u8(0x01);
    writer.write_u8(0x02);
    const auto data = writer.take();

    Reader reader(data);
    REQUIRE(reader.read_u8() == 0x01);
    REQUIRE_THROWS_AS(reader.expect_end(), Error);
}

// ---------------------------------------------------------------------------
// Description: Size and vector helpers round trip through a Reader
// Rationale:   The shared helpers must reproduce the section wire format
// ---------------------------------------------------------------------------
TEST_CASE("Size and vector helpers round trip", "[serialize]")
{
    const std::vector<std::uint64_t> timestamps = {
        0, 1, 0x0123456789ABCDEFULL};
    const std::vector<std::uint8_t> flags = {0, 1, 0xFF};
    const std::vector<bool> populated = {true, false, true};

    Writer writer;
    ssp4sim::serialize::write_size(writer, 42);
    ssp4sim::serialize::write_u64_vector(writer, timestamps);
    ssp4sim::serialize::write_u8_vector(writer, flags);
    ssp4sim::serialize::write_bool_vector(writer, populated);

    const auto data = writer.take();
    Reader reader(data);

    REQUIRE(ssp4sim::serialize::read_size(reader) == 42);
    REQUIRE(ssp4sim::serialize::read_u64_vector(reader) == timestamps);
    REQUIRE(ssp4sim::serialize::read_u8_vector(reader) == flags);
    REQUIRE(ssp4sim::serialize::read_bool_vector(reader) == populated);
    reader.expect_end();
}

// ---------------------------------------------------------------------------
// Description: Masked blobs zero skipped ranges on write and leave them on read
// Rationale:   In-place string slots must survive a snapshot round trip
// ---------------------------------------------------------------------------
TEST_CASE("Masked blob zeroes and skips ranges", "[serialize]")
{
    const std::array<std::byte, 8> blob = {
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    const std::array<ByteRange, 2> mask = {ByteRange{2, 1}, ByteRange{3, 2}};

    Writer writer;
    ssp4sim::serialize::write_masked_blob(writer, blob, mask);
    const auto data = writer.take();

    const std::array<std::byte, 8> encoded = {
        std::byte{0x10}, std::byte{0x11}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    REQUIRE(data.size() == sizeof(std::uint64_t) + encoded.size());
    REQUIRE(std::equal(data.begin() + sizeof(std::uint64_t), data.end(),
                       encoded.begin(), encoded.end()));

    Reader reader(data);
    std::array<std::byte, 8> target;
    target.fill(std::byte{0xAA});
    ssp4sim::serialize::read_masked_blob(reader, target, mask);
    reader.expect_end();

    const std::array<std::byte, 8> expected = {
        std::byte{0x10}, std::byte{0x11}, std::byte{0xAA}, std::byte{0xAA},
        std::byte{0xAA}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    REQUIRE(std::equal(target.begin(), target.end(), expected.begin(),
                       expected.end()));
}

// ---------------------------------------------------------------------------
// Description: read_count rejects counts larger than the remaining input
// Rationale:   A forged count must not drive a huge allocation
// ---------------------------------------------------------------------------
TEST_CASE("read_count rejects counts beyond remaining input", "[serialize]")
{
    Writer writer;
    ssp4sim::serialize::write_size(writer, 5);
    writer.write_u8(0);
    const auto data = writer.take();

    Reader reader(data);
    REQUIRE_THROWS_AS(ssp4sim::serialize::read_count(reader, 2), Error);
}

// ---------------------------------------------------------------------------
// Description: Masked blob length and ranges are validated before the copy
// Rationale:   A bad section must not mutate the target buffer
// ---------------------------------------------------------------------------
TEST_CASE("Masked blob rejects bad length and ranges", "[serialize]")
{
    const std::array<std::byte, 4> blob = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    const std::array<ByteRange, 1> outside = {ByteRange{2, 3}};

    Writer bad_range;
    REQUIRE_THROWS_AS(
        ssp4sim::serialize::write_masked_blob(bad_range, blob, outside), Error);

    Writer writer;
    ssp4sim::serialize::write_masked_blob(writer, blob, {});
    const auto data = writer.take();

    Reader reader(data);
    std::array<std::byte, 3> wrong_size;
    wrong_size.fill(std::byte{0xAA});
    REQUIRE_THROWS_AS(
        ssp4sim::serialize::read_masked_blob(reader, wrong_size, {}), Error);
    REQUIRE(std::all_of(wrong_size.begin(), wrong_size.end(),
                        [](std::byte value) { return value == std::byte{0xAA}; }));
}

// ---------------------------------------------------------------------------
// Description: require_valid_text accepts strict UTF-8 and rejects NUL and
//              malformed sequences with distinct messages
// Rationale:   FMI strings must round-trip as valid UTF-8 without embedded NUL
// ---------------------------------------------------------------------------
TEST_CASE("require_valid_text enforces UTF-8 and rejects NUL", "[serialize]")
{
    using ssp4sim::serialize::require_valid_text;

    REQUIRE_NOTHROW(require_valid_text("plain", "string variable 'x'"));
    REQUIRE_NOTHROW(require_valid_text("", "string variable 'x'"));
    REQUIRE_NOTHROW(require_valid_text(utf8(u8"åäö"), "string variable 'x'"));
    REQUIRE_NOTHROW(require_valid_text(utf8(u8"😀"), "string variable 'x'"));

    try
    {
        require_valid_text(std::string("a\0b", 3), "string variable 'x'");
        FAIL("expected serialize::Unsupported");
    }
    catch (const Unsupported &error)
    {
        REQUIRE(std::string(error.what()) ==
                "serialize: string variable 'x' contains NUL");
    }

    const std::array<std::string_view, 5> invalid = {
        "\x80", "\xC0\x80", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80"};
    for (const std::string_view value : invalid)
    {
        try
        {
            require_valid_text(value, "string variable 'x'");
            FAIL("expected serialize::Unsupported");
        }
        catch (const Unsupported &error)
        {
            REQUIRE(std::string(error.what()) ==
                    "serialize: string variable 'x' is not valid UTF-8");
        }
    }
}

// ---------------------------------------------------------------------------
// Description: write_text and read_text round trip valid text and reject
//              invalid text before writing
// Rationale:   The text codec is the single validation point for text fields
// ---------------------------------------------------------------------------
TEST_CASE("write_text and read_text round trip text", "[serialize]")
{
    const std::string expected(utf8(u8"åäö😀"));

    Writer writer;
    ssp4sim::serialize::write_text(writer, expected, "text.value");
    const auto data = writer.take();

    Reader reader(data);
    REQUIRE(ssp4sim::serialize::read_text(reader, "text.value") == expected);
    reader.expect_end();

    Writer invalid_nul;
    REQUIRE_THROWS_AS(
        ssp4sim::serialize::write_text(invalid_nul,
                                       std::string("a\0b", 3), "text.value"),
        Unsupported);

    Writer invalid_utf8;
    REQUIRE_THROWS_AS(
        ssp4sim::serialize::write_text(invalid_utf8, "\x80", "text.value"),
        Unsupported);
}

// ---------------------------------------------------------------------------
// Description: require_equal throws a consistent message only on mismatch
// Rationale:   Identity errors must name the field and both values
// ---------------------------------------------------------------------------
TEST_CASE("require_equal reports field mismatches", "[serialize]")
{
    REQUIRE_NOTHROW(
        ssp4sim::serialize::require_equal("storage.name", "same", "same"));
    REQUIRE_NOTHROW(
        ssp4sim::serialize::require_equal("size", std::size_t{3}, std::size_t{3}));

    try
    {
        ssp4sim::serialize::require_equal("storage.name", "live", "saved");
        FAIL("expected serialize::Error");
    }
    catch (const Error &error)
    {
        REQUIRE(std::string(error.what()) ==
                "serialize: storage.name mismatch: live=live, saved=saved");
    }

    REQUIRE_THROWS_AS(
        ssp4sim::serialize::require_equal("size", std::size_t{1}, std::size_t{2}),
        Error);
}

// ---------------------------------------------------------------------------
// Description: StringTable writes the canonical count/area/index/name/value
//              bytes in insertion order and reads them back unchanged
// Rationale:   The v3 wire format must not depend on entry ordering or count
// ---------------------------------------------------------------------------
TEST_CASE("StringTable round trips arbitrary entries in insertion order",
          "[serialize]")
{
    ssp4sim::serialize::StringTable table;
    table.add(1, 7, "second", "value-b");
    table.add(0, 2, "first", "");
    table.add(5, 0, "third", std::string(utf8(u8"åäö😀")));
    REQUIRE(table.size() == 3);

    Writer expected;
    ssp4sim::serialize::write_size(expected, 3);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> keys = {
        std::pair<std::uint64_t, std::uint64_t>{1, 7},
        std::pair<std::uint64_t, std::uint64_t>{0, 2},
        std::pair<std::uint64_t, std::uint64_t>{5, 0}};
    const std::array<std::string, 3> names = {"second", "first", "third"};
    const std::array<std::string, 3> values = {
        "value-b", "", std::string(utf8(u8"åäö😀"))};
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        ssp4sim::serialize::write_size(expected, keys[i].first);
        ssp4sim::serialize::write_size(expected, keys[i].second);
        ssp4sim::serialize::write_text(expected, names[i], "string table name");
        ssp4sim::serialize::write_text(
            expected, values[i], "string table value '" + names[i] + "'");
    }

    Writer writer;
    table.write(writer);
    const auto data = writer.take();
    REQUIRE(data == expected.take());

    Reader reader(data);
    const auto restored = ssp4sim::serialize::StringTable::read(reader);
    reader.expect_end();
    REQUIRE(restored.size() == 3);
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const auto *entry = restored.find(keys[i].first, keys[i].second);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->area == keys[i].first);
        REQUIRE(entry->index == keys[i].second);
        REQUIRE(entry->name == names[i]);
        REQUIRE(entry->value == values[i]);
    }

    Writer empty_writer;
    ssp4sim::serialize::write_size(empty_writer, 0);
    const auto empty_data = empty_writer.take();
    Reader empty_reader(empty_data);
    const auto empty = ssp4sim::serialize::StringTable::read(empty_reader);
    empty_reader.expect_end();
    REQUIRE(empty.size() == 0);
}

// ---------------------------------------------------------------------------
// Description: StringTable rejects duplicate keys and invalid text before
//              storing an entry
// Rationale:   Duplicate detection and text validation belong to the codec
// ---------------------------------------------------------------------------
TEST_CASE("StringTable rejects duplicate keys and invalid text",
          "[serialize]")
{
    ssp4sim::serialize::StringTable table;
    table.add(1, 2, "name", "value");
    REQUIRE_THROWS_AS(table.add(1, 2, "other", "other"), Error);
    REQUIRE(table.size() == 1);

    REQUIRE_THROWS_AS(table.add(3, 4, std::string("a\0b", 3), "value"),
                      Unsupported);
    REQUIRE_THROWS_AS(table.add(3, 4, "name", std::string("a\0b", 3)),
                      Unsupported);
    REQUIRE_THROWS_AS(table.add(3, 4, "name", "\x80"), Unsupported);
    REQUIRE(table.size() == 1);

    // A duplicate key inside the input is rejected by read() via add().
    Writer duplicate;
    ssp4sim::serialize::write_size(duplicate, 2);
    for (int i = 0; i < 2; ++i)
    {
        ssp4sim::serialize::write_size(duplicate, 0);
        ssp4sim::serialize::write_size(duplicate, 0);
        ssp4sim::serialize::write_text(duplicate, "name", "string table name");
        ssp4sim::serialize::write_text(duplicate, "value",
                                       "string table value 'name'");
    }
    const auto duplicate_data = duplicate.take();
    Reader duplicate_reader(duplicate_data);
    REQUIRE_THROWS_AS(ssp4sim::serialize::StringTable::read(duplicate_reader),
                      Error);
}

// ---------------------------------------------------------------------------
// Description: StringTable find() distinguishes present and absent slots and
//              works through both overloads
// Rationale:   Coverage checks rely on a null result for a missing slot
// ---------------------------------------------------------------------------
TEST_CASE("StringTable find returns null for a missing slot", "[serialize]")
{
    ssp4sim::serialize::StringTable table;
    table.add(0, 1, "present", "value");

    REQUIRE(table.find(0, 1) != nullptr);
    REQUIRE(table.find(0, 2) == nullptr);
    REQUIRE(table.find(1, 1) == nullptr);

    const auto &const_table = table;
    REQUIRE(const_table.find(0, 1) != nullptr);
    REQUIRE(const_table.find(0, 1)->value == "value");
    REQUIRE(const_table.find(9, 9) == nullptr);
}
