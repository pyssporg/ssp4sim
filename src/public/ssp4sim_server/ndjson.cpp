#include "ndjson.hpp"

#include <algorithm>
#include <cstring>

namespace ssp4simd
{
    LineFramer::LineFramer(std::size_t max_line)
        : max_line_(max_line)
    {
        buffer_.reserve(4096);
    }

    void LineFramer::feed(const char *data, std::size_t size,
                          const LineFn &on_line, const TooLongFn &on_too_long)
    {
        std::size_t offset = 0;

        while (offset < size)
        {
            if (discarding_)
            {
                const char *newline = static_cast<const char *>(
                    std::memchr(data + offset, '\n', size - offset));
                if (newline == nullptr)
                {
                    return;
                }
                offset = static_cast<std::size_t>(newline - data) + 1;
                discarding_ = false;
                continue;
            }

            const char *newline = static_cast<const char *>(
                std::memchr(data + offset, '\n', size - offset));
            const std::size_t segment_end =
                newline == nullptr ? size : static_cast<std::size_t>(newline - data);

            if (newline == nullptr)
            {
                if (buffer_.size() + (segment_end - offset) > max_line_)
                {
                    buffer_.clear();
                    discarding_ = true;
                    on_too_long();
                    return;
                }
                buffer_.append(data + offset, segment_end - offset);
                return;
            }

            const std::size_t line_size = buffer_.size() + (segment_end - offset);
            if (line_size > max_line_)
            {
                buffer_.clear();
                offset = segment_end + 1;
                on_too_long();
                continue;
            }

            buffer_.append(data + offset, segment_end - offset);
            offset = segment_end + 1;

            std::string_view line(buffer_);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            on_line(line);
            buffer_.clear();
        }
    }
}
