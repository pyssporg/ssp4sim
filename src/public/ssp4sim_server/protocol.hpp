#pragma once

#include "runtime_control.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ssp4simd
{
    using json = nlohmann::json;

    struct ParseResult
    {
        bool ok = false;
        json id = nullptr;
        json request;
        std::string code;
        std::string message;
    };

    /// Parse and validate the protocol envelope (UTF-8 JSON object, "v":1,
    /// string "cmd"). Command-specific fields are validated by the daemon.
    ParseResult parse_request(std::string_view line);

    json make_ok(const json &id);
    json make_error(const json &id, const std::string &code,
                    const std::string &message);

    const char *state_to_string(ssp4sim::RunState state);
    const char *result_to_string(ssp4sim::RunResult result);

    double ns_to_seconds(uint64_t value);

    /// Optional finite numeric field. Returns false when absent, non-numeric
    /// or non-finite.
    bool get_number_field(const json &request, const char *name, double &out);

    /// Required non-empty string field.
    bool get_string_field(const json &request, const char *name,
                          std::string &out);

    /// Required "input" or "output" field.
    bool get_side_field(const json &request, ssp4sim::VariableSide &out);

    /// Parse the optional "at" field (simulation seconds) into nanoseconds.
    /// Absent or null leaves \p out empty. Returns false with \p message set
    /// when "at" is present but not a finite number >= 0 or does not fit in
    /// uint64_t nanoseconds.
    bool get_at_ns_field(const json &request, std::optional<uint64_t> &out,
                         std::string &message);
}
