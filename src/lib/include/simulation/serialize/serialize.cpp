#include "simulation/serialize/serialize.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace ssp4sim::serialize
{
    namespace
    {
        void require_available(std::size_t remaining, std::size_t requested)
        {
            if (requested > remaining)
            {
                throw Error("serialize: truncated input (" + std::to_string(requested) +
                            " bytes requested, " + std::to_string(remaining) + " available)");
            }
        }

        void append(std::vector<std::byte> &out, const void *source, std::size_t size)
        {
            if (size == 0)
            {
                return;
            }
            const auto *bytes = static_cast<const std::byte *>(source);
            out.insert(out.end(), bytes, bytes + size);
        }

        /// Validate ranges against size, then sort and merge overlapping or
        /// adjacent ranges.
        std::vector<ByteRange> normalize_ranges(
            std::span<const ByteRange> ranges, std::size_t size)
        {
            for (const ByteRange &range : ranges)
            {
                if (range.offset > size || range.size > size - range.offset)
                {
                    throw Error("serialize: byte range lies outside the blob");
                }
            }

            std::vector<ByteRange> sorted(ranges.begin(), ranges.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const ByteRange &a, const ByteRange &b)
                      {
                          return a.offset < b.offset;
                      });

            std::vector<ByteRange> merged;
            for (const ByteRange &range : sorted)
            {
                if (!merged.empty() &&
                    range.offset <= merged.back().offset + merged.back().size)
                {
                    const std::size_t end =
                        std::max(merged.back().offset + merged.back().size,
                                 range.offset + range.size);
                    merged.back().size = end - merged.back().offset;
                    continue;
                }
                merged.push_back(range);
            }
            return merged;
        }
    }

    void Writer::write_u8(std::uint8_t value)
    {
        const std::byte byte = static_cast<std::byte>(value);
        bytes_.push_back(byte);
    }

    void Writer::write_u32(std::uint32_t value)
    {
        std::byte encoded[4];
        for (std::size_t i = 0; i < sizeof(encoded); ++i)
        {
            encoded[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
        }
        append(bytes_, encoded, sizeof(encoded));
    }

    void Writer::write_u64(std::uint64_t value)
    {
        std::byte encoded[8];
        for (std::size_t i = 0; i < sizeof(encoded); ++i)
        {
            encoded[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
        }
        append(bytes_, encoded, sizeof(encoded));
    }

    void Writer::write_string(std::string_view value)
    {
        write_u64(static_cast<std::uint64_t>(value.size()));
        append(bytes_, value.data(), value.size());
    }

    void Writer::write_bytes(std::span<const std::byte> value)
    {
        append(bytes_, value.data(), value.size());
    }

    void Writer::write_blob(std::span<const std::byte> value)
    {
        write_u64(static_cast<std::uint64_t>(value.size()));
        write_bytes(value);
    }

    Reader::Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes)
    {
    }

    std::uint8_t Reader::read_u8()
    {
        return static_cast<std::uint8_t>(read_bytes(1).front());
    }

    std::uint32_t Reader::read_u32()
    {
        const auto encoded = read_bytes(4);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < encoded.size(); ++i)
        {
            value |= static_cast<std::uint32_t>(
                         static_cast<unsigned char>(encoded[i]))
                     << (8 * i);
        }
        return value;
    }

    std::uint64_t Reader::read_u64()
    {
        const auto encoded = read_bytes(8);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < encoded.size(); ++i)
        {
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(encoded[i]))
                     << (8 * i);
        }
        return value;
    }

    std::string Reader::read_string()
    {
        const std::uint64_t size = read_u64();
        if (size > std::numeric_limits<std::size_t>::max())
        {
            throw Error("serialize: string length does not fit size_t");
        }
        const auto bytes = read_bytes(static_cast<std::size_t>(size));
        if (bytes.empty())
        {
            return {};
        }
        return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    std::span<const std::byte> Reader::read_bytes(std::size_t size)
    {
        require_available(remaining(), size);
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    std::span<const std::byte> Reader::read_blob()
    {
        const std::uint64_t size = read_u64();
        if (size > std::numeric_limits<std::size_t>::max())
        {
            throw Error("serialize: blob length does not fit size_t");
        }
        return read_bytes(static_cast<std::size_t>(size));
    }

    void Reader::expect_end() const
    {
        if (remaining() != 0)
        {
            throw Error("serialize: " + std::to_string(remaining()) +
                        " trailing bytes after section");
        }
    }

    void write_size(Writer &out, std::size_t value)
    {
        out.write_u64(static_cast<std::uint64_t>(value));
    }

    std::size_t read_size(Reader &in)
    {
        const std::uint64_t size = in.read_u64();
        if (size > std::numeric_limits<std::size_t>::max())
        {
            throw Error("serialize: size does not fit size_t");
        }
        return static_cast<std::size_t>(size);
    }

    std::size_t read_count(Reader &in, std::size_t minimum_entry_size)
    {
        const std::size_t count = read_size(in);
        if (minimum_entry_size != 0 &&
            count > in.remaining() / minimum_entry_size)
        {
            throw Error("serialize: count exceeds remaining input (" +
                        std::to_string(count) + " entries, " +
                        std::to_string(in.remaining()) + " bytes available)");
        }
        return count;
    }

    void write_u64_vector(Writer &out, std::span<const std::uint64_t> values)
    {
        write_size(out, values.size());
        for (const std::uint64_t value : values)
        {
            out.write_u64(value);
        }
    }

    std::vector<std::uint64_t> read_u64_vector(Reader &in)
    {
        const std::size_t count = read_count(in, sizeof(std::uint64_t));
        std::vector<std::uint64_t> values(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            values[i] = in.read_u64();
        }
        return values;
    }

    void write_u8_vector(Writer &out, std::span<const std::uint8_t> values)
    {
        write_size(out, values.size());
        for (const std::uint8_t value : values)
        {
            out.write_u8(value);
        }
    }

    std::vector<std::uint8_t> read_u8_vector(Reader &in)
    {
        const std::size_t count = read_count(in, sizeof(std::uint8_t));
        std::vector<std::uint8_t> values(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            values[i] = in.read_u8();
        }
        return values;
    }

    void write_bool_vector(Writer &out, const std::vector<bool> &values)
    {
        write_size(out, values.size());
        for (const bool value : values)
        {
            out.write_u8(value ? std::uint8_t{1} : std::uint8_t{0});
        }
    }

    std::vector<bool> read_bool_vector(Reader &in)
    {
        const std::size_t count = read_count(in, sizeof(std::uint8_t));
        std::vector<bool> values(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            values[i] = in.read_u8() != 0;
        }
        return values;
    }

    void write_masked_blob(Writer &out, std::span<const std::byte> blob,
                           std::span<const ByteRange> zero_ranges)
    {
        const std::vector<ByteRange> ranges =
            normalize_ranges(zero_ranges, blob.size());

        write_size(out, blob.size());

        std::size_t cursor = 0;
        for (const ByteRange &range : ranges)
        {
            out.write_bytes(blob.subspan(cursor, range.offset - cursor));
            for (std::size_t i = 0; i < range.size; ++i)
            {
                out.write_u8(0);
            }
            cursor = range.offset + range.size;
        }
        out.write_bytes(blob.subspan(cursor));
    }

    void read_masked_blob(Reader &in, std::span<std::byte> target,
                          std::span<const ByteRange> skip_ranges)
    {
        const std::size_t size = read_size(in);
        if (size != target.size())
        {
            throw Error("serialize: blob size mismatch: live=" +
                        std::to_string(target.size()) +
                        ", saved=" + std::to_string(size));
        }

        const std::vector<ByteRange> ranges =
            normalize_ranges(skip_ranges, target.size());

        const std::span<const std::byte> source = in.read_bytes(target.size());

        std::size_t cursor = 0;
        for (const ByteRange &range : ranges)
        {
            std::copy_n(source.begin() + cursor, range.offset - cursor,
                        target.begin() + cursor);
            cursor = range.offset + range.size;
        }
        std::copy_n(source.begin() + cursor, target.size() - cursor,
                    target.begin() + cursor);
    }

    void require_equal(std::string_view field, std::string_view live,
                       std::string_view saved)
    {
        if (live != saved)
        {
            throw Error("serialize: " + std::string(field) +
                        " mismatch: live=" + std::string(live) +
                        ", saved=" + std::string(saved));
        }
    }

    void require_equal(std::string_view field, std::size_t live,
                       std::size_t saved)
    {
        if (live != saved)
        {
            throw Error("serialize: " + std::string(field) +
                        " mismatch: live=" + std::to_string(live) +
                        ", saved=" + std::to_string(saved));
        }
    }

    bool is_valid_utf8(std::string_view value) noexcept
    {
        std::size_t i = 0;
        const std::size_t size = value.size();
        while (i < size)
        {
            const auto byte = static_cast<unsigned char>(value[i]);
            if (byte <= 0x7F)
            {
                ++i;
                continue;
            }

            std::size_t continuation = 0;
            unsigned char second_min = 0x80;
            unsigned char second_max = 0xBF;
            if (byte >= 0xC2 && byte <= 0xDF)
            {
                continuation = 1;
            }
            else if (byte == 0xE0)
            {
                continuation = 2;
                second_min = 0xA0;
            }
            else if (byte >= 0xE1 && byte <= 0xEC)
            {
                continuation = 2;
            }
            else if (byte == 0xED)
            {
                continuation = 2;
                second_max = 0x9F;
            }
            else if (byte >= 0xEE && byte <= 0xEF)
            {
                continuation = 2;
            }
            else if (byte == 0xF0)
            {
                continuation = 3;
                second_min = 0x90;
            }
            else if (byte >= 0xF1 && byte <= 0xF3)
            {
                continuation = 3;
            }
            else if (byte == 0xF4)
            {
                continuation = 3;
                second_max = 0x8F;
            }
            else
            {
                return false;
            }

            if (i + continuation >= size)
            {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if (second < second_min || second > second_max)
            {
                return false;
            }
            for (std::size_t j = 2; j <= continuation; ++j)
            {
                const auto next = static_cast<unsigned char>(value[i + j]);
                if (next < 0x80 || next > 0xBF)
                {
                    return false;
                }
            }
            i += continuation + 1;
        }
        return true;
    }

    void require_valid_text(std::string_view value, std::string_view field)
    {
        if (value.find('\0') != std::string_view::npos)
        {
            throw Unsupported("serialize: " + std::string(field) +
                              " contains NUL");
        }
        if (!is_valid_utf8(value))
        {
            throw Unsupported("serialize: " + std::string(field) +
                              " is not valid UTF-8");
        }
    }

    void write_text(Writer &out, std::string_view value, std::string_view field)
    {
        require_valid_text(value, field);
        out.write_string(value);
    }

    std::string read_text(Reader &in, std::string_view field)
    {
        std::string value = in.read_string();
        require_valid_text(value, field);
        return value;
    }

    void StringTable::add(std::uint64_t area, std::uint64_t index,
                          std::string name, std::string value)
    {
        require_valid_text(name, "string table name");
        require_valid_text(value, "string table value '" + name + "'");

        const auto key = std::make_pair(area, index);
        if (index_.find(key) != index_.end())
        {
            throw Error("serialize: duplicate string table entry (" +
                        std::to_string(area) + "," + std::to_string(index) +
                        ")");
        }

        index_.emplace(key, entries_.size());
        entries_.push_back(
            StringEntry{area, index, std::move(name), std::move(value)});
    }

    StringEntry *StringTable::find(std::uint64_t area,
                                   std::uint64_t index) noexcept
    {
        const auto it = index_.find({area, index});
        if (it == index_.end())
        {
            return nullptr;
        }
        return &entries_[it->second];
    }

    const StringEntry *StringTable::find(std::uint64_t area,
                                         std::uint64_t index) const noexcept
    {
        const auto it = index_.find({area, index});
        if (it == index_.end())
        {
            return nullptr;
        }
        return &entries_[it->second];
    }

    void StringTable::write(Writer &out) const
    {
        write_size(out, entries_.size());
        for (const auto &entry : entries_)
        {
            out.write_u64(entry.area);
            out.write_u64(entry.index);
            out.write_string(entry.name);
            out.write_string(entry.value);
        }
    }

    StringTable StringTable::read(Reader &in)
    {
        const std::size_t count = read_count(in, 4 * sizeof(std::uint64_t));
        StringTable table;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint64_t area = in.read_u64();
            const std::uint64_t index = in.read_u64();
            const std::string name = in.read_string();
            std::string value = in.read_string();
            table.add(area, index, name, std::move(value));
        }
        return table;
    }
}
