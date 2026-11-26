#include "ndjson.hpp"
#include "protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using ssp4simd::LineFramer;
using ssp4simd::parse_request;

namespace
{
    struct Collector
    {
        std::vector<std::string> lines;
        int too_long = 0;

        LineFramer::LineFn on_line()
        {
            return [this](std::string_view line)
            { lines.emplace_back(line); };
        }

        LineFramer::TooLongFn on_too_long()
        {
            return [this] { ++too_long; };
        }
    };
}

TEST_CASE("framer splits complete lines and keeps partial data", "[ndjson]")
{
    LineFramer framer(64);
    Collector collector;

    const std::string first = "{\"a\":1}\n{\"b\":2}\n{\"c\":";
    framer.feed(first.data(), first.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.lines.size() == 2);
    REQUIRE(collector.lines[0] == "{\"a\":1}");
    REQUIRE(collector.lines[1] == "{\"b\":2}");

    const std::string rest = "3}\n";
    framer.feed(rest.data(), rest.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.lines.size() == 3);
    REQUIRE(collector.lines[2] == "{\"c\":3}");
    REQUIRE(collector.too_long == 0);
}

TEST_CASE("framer strips a trailing carriage return", "[ndjson]")
{
    LineFramer framer(64);
    Collector collector;

    const std::string line = "{\"x\":1}\r\n";
    framer.feed(line.data(), line.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.lines.size() == 1);
    REQUIRE(collector.lines[0] == "{\"x\":1}");
}

TEST_CASE("framer reports an oversized line once and resynchronises", "[ndjson]")
{
    LineFramer framer(8);
    Collector collector;

    const std::string oversized = "123456789\n{\"ok\":1}\n";
    framer.feed(oversized.data(), oversized.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.too_long == 1);
    REQUIRE(collector.lines.size() == 1);
    REQUIRE(collector.lines[0] == "{\"ok\":1}");
}

TEST_CASE("framer discards until newline after an unterminated oversized line",
          "[ndjson]")
{
    LineFramer framer(8);
    Collector collector;

    const std::string chunk = "abcdefghij";
    framer.feed(chunk.data(), chunk.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.too_long == 1);
    REQUIRE(collector.lines.empty());
    REQUIRE(framer.discarding());

    const std::string tail = "klmn\n{\"ok\":1}\n";
    framer.feed(tail.data(), tail.size(), collector.on_line(),
                collector.on_too_long());

    REQUIRE(collector.too_long == 1);
    REQUIRE(collector.lines.size() == 1);
    REQUIRE(collector.lines[0] == "{\"ok\":1}");
    REQUIRE_FALSE(framer.discarding());
}

TEST_CASE("request parser validates the envelope", "[protocol]")
{
    const auto valid = parse_request("{\"v\":1,\"id\":7,\"cmd\":\"state\"}");
    REQUIRE(valid.ok);
    REQUIRE(valid.id == 7);
    REQUIRE(valid.request["cmd"] == "state");

    const auto malformed = parse_request("{");
    REQUIRE_FALSE(malformed.ok);
    REQUIRE(malformed.code == "bad_json");
    REQUIRE(malformed.id.is_null());

    const auto array = parse_request("[1,2,3]");
    REQUIRE_FALSE(array.ok);
    REQUIRE(array.code == "bad_request");

    const auto wrong_version = parse_request("{\"v\":2,\"id\":1,\"cmd\":\"state\"}");
    REQUIRE_FALSE(wrong_version.ok);
    REQUIRE(wrong_version.code == "bad_version");
    REQUIRE(wrong_version.id == 1);

    const auto missing_command = parse_request("{\"v\":1,\"id\":1}");
    REQUIRE_FALSE(missing_command.ok);
    REQUIRE(missing_command.code == "bad_request");

    // Envelope errors do not reject unknown commands; the daemon does that.
    const auto unknown = parse_request("{\"v\":1,\"id\":1,\"cmd\":\"nope\"}");
    REQUIRE(unknown.ok);
    REQUIRE(unknown.request["cmd"] == "nope");
}

