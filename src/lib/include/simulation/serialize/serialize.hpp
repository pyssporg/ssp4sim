#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssp4sim::serialize
{
    /// Malformed or truncated serialized state.
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// State that the serialization format cannot represent, for example a
    /// string containing NUL or invalid UTF-8, which FMI strings cannot
    /// round-trip.
    class Unsupported : public Error
    {
    public:
        using Error::Error;
    };

    /// Append-only little-endian byte writer. Lengths are u64; raw byte
    /// spans are written verbatim.
    class Writer
    {
    public:
        void write_u8(std::uint8_t value);
        void write_u32(std::uint32_t value);
        void write_u64(std::uint64_t value);
        void write_string(std::string_view value);
        void write_bytes(std::span<const std::byte> value);
        void write_blob(std::span<const std::byte> value);

        const std::vector<std::byte> &data() const noexcept { return bytes_; }
        std::vector<std::byte> take() noexcept { return std::move(bytes_); }

    private:
        std::vector<std::byte> bytes_;
    };

    /// Bounds-checked reader over one serialized section.
    class Reader
    {
    public:
        explicit Reader(std::span<const std::byte> bytes) noexcept;

        std::uint8_t read_u8();
        std::uint32_t read_u32();
        std::uint64_t read_u64();
        std::string read_string();
        std::span<const std::byte> read_bytes(std::size_t size);
        std::span<const std::byte> read_blob();

        std::size_t remaining() const noexcept { return bytes_.size() - position_; }
        void expect_end() const;

    private:
        std::span<const std::byte> bytes_;
        std::size_t position_ = 0;
    };

    /// Masked byte range used to leave in-place string slots untouched.
    struct ByteRange
    {
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    /// Write a size_t as a u64 wire value.
    void write_size(Writer &out, std::size_t value);

    /// Read a u64 wire value as a size_t, rejecting values that do not fit.
    std::size_t read_size(Reader &in);

    /// Read a u64 element count, rejecting values that do not fit size_t and
    /// counts larger than the remaining input could hold. minimum_entry_size
    /// is a lower bound for one encoded element, used only for the bound
    /// check; it may be zero to skip that check.
    std::size_t read_count(Reader &in, std::size_t minimum_entry_size);

    void write_u64_vector(Writer &out, std::span<const std::uint64_t> values);
    std::vector<std::uint64_t> read_u64_vector(Reader &in);

    void write_u8_vector(Writer &out, std::span<const std::uint8_t> values);
    std::vector<std::uint8_t> read_u8_vector(Reader &in);

    void write_bool_vector(Writer &out, const std::vector<bool> &values);
    std::vector<bool> read_bool_vector(Reader &in);

    /// Write blob.size() followed by the blob, with every byte covered by the
    /// normalized ranges replaced by zero. Ranges must lie inside the blob.
    void write_masked_blob(Writer &out, std::span<const std::byte> blob,
                           std::span<const ByteRange> zero_ranges);

    /// Read a blob whose length must equal target.size() and copy every byte
    /// not covered by the normalized skip_ranges into target. Skipped ranges
    /// are left untouched. All validation happens before target is mutated.
    void read_masked_blob(Reader &in, std::span<std::byte> target,
                          std::span<const ByteRange> skip_ranges);

    /// Throw Error unless live equals saved, with a consistent field message.
    void require_equal(std::string_view field, std::string_view live,
                       std::string_view saved);
    void require_equal(std::string_view field, std::size_t live,
                       std::size_t saved);

    /// Throw Unsupported unless value is strict UTF-8 without embedded NUL.
    void require_valid_text(std::string_view value, std::string_view field);

    /// Strict UTF-8 well-formedness: rejects overlong encodings, lone
    /// continuation bytes, truncated sequences, UTF-16 surrogates, and code
    /// points above U+10FFFF. Accepts ASCII and the empty string.
    bool is_valid_utf8(std::string_view value) noexcept;

    /// Validate value as text, then write it as a length-prefixed string.
    void write_text(Writer &out, std::string_view value, std::string_view field);

    /// Read a length-prefixed string, then validate it as text.
    std::string read_text(Reader &in, std::string_view field);

    /// One validated string-table entry: an (area, index) slot addressed by
    /// name plus the text value stored in it.
    struct StringEntry
    {
        std::uint64_t area = 0;
        std::uint64_t index = 0;
        std::string name;
        std::string value;
    };

    /// Unique (area, index) string slots with validated text, written as
    /// count followed by area, index, name, value in insertion order.
    class StringTable
    {
    public:
        /// Validate name and value as text, then append the entry. Throws
        /// Error when (area, index) is already present.
        void add(std::uint64_t area, std::uint64_t index, std::string name,
                 std::string value);

        std::size_t size() const noexcept { return entries_.size(); }

        StringEntry *find(std::uint64_t area, std::uint64_t index) noexcept;
        const StringEntry *find(std::uint64_t area,
                                std::uint64_t index) const noexcept;

        /// Write the count and every entry in insertion order.
        void write(Writer &out) const;

        /// Read a table, validating every entry through add().
        static StringTable read(Reader &in);

    private:
        std::vector<StringEntry> entries_;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> index_;
    };

    /// State-bearing type that can write and read its own section.
    class ISerialize
    {
    public:
        virtual ~ISerialize() = default;

        virtual void serialize(Writer &out) const = 0;
        virtual void deserialize(Reader &in) = 0;
    };
}
