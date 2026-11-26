#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace ssp4simd
{
    /// Incremental newline-delimited framing. Feed raw bytes; complete lines are
    /// handed to the line callback without their trailing newline (and without a
    /// trailing carriage return). A line longer than the configured maximum is
    /// reported once and discarded up to the next newline so the stream can
    /// resynchronise.
    class LineFramer
    {
    public:
        using LineFn = std::function<void(std::string_view)>;
        using TooLongFn = std::function<void()>;

        static constexpr std::size_t default_max_line = 1024u * 1024u;

        explicit LineFramer(std::size_t max_line = default_max_line);

        void feed(const char *data, std::size_t size,
                  const LineFn &on_line, const TooLongFn &on_too_long);

        bool discarding() const noexcept { return discarding_; }

    private:
        std::string buffer_;
        std::size_t max_line_;
        bool discarding_ = false;
    };
}
