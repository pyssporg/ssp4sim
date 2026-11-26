#pragma once

#include "runtime_control.hpp"

#include <cstdint>
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
}
