#include "protocol.hpp"

#include "c_api/simulator_c_api.h"

#include <cmath>

namespace ssp4simd
{
    ParseResult parse_request(std::string_view line)
    {
        ParseResult result;

        json parsed;
        try
        {
            parsed = json::parse(line.begin(), line.end());
        }
        catch (const json::exception &)
        {
            result.code = "bad_json";
            result.message = "request is not valid JSON";
            return result;
        }

        if (!parsed.is_object())
        {
            result.code = "bad_request";
            result.message = "request must be a JSON object";
            return result;
        }

        if (parsed.contains("id"))
        {
            result.id = parsed["id"];
        }

        result.request = std::move(parsed);

        if (!result.request.contains("v") ||
            !result.request["v"].is_number_integer() ||
            result.request["v"].get<int>() != 1)
        {
            result.code = "bad_version";
            result.message = "unsupported or missing protocol version \"v\":1";
            return result;
        }

        if (!result.request.contains("cmd") || !result.request["cmd"].is_string())
        {
            result.code = "bad_request";
            result.message = "missing string field \"cmd\"";
            return result;
        }

        result.ok = true;
        return result;
    }

    json make_ok(const json &id)
    {
        json response;
        response["v"] = 1;
        response["id"] = id;
        response["ok"] = true;
        return response;
    }

    json make_error(const json &id, const std::string &code,
                    const std::string &message)
    {
        json response;
        response["v"] = 1;
        response["id"] = id;
        response["ok"] = false;
        response["error"] = json{{"code", code}, {"message", message}};
        return response;
    }

    const char *state_to_string(ssp4sim::RunState state)
    {
        return ssp4sim_run_state_name(static_cast<ssp4sim_run_state>(state));
    }

    const char *result_to_string(ssp4sim::RunResult result)
    {
        return ssp4sim_run_result_name(
            static_cast<ssp4sim_run_result>(result));
    }

    double ns_to_seconds(uint64_t value)
    {
        return static_cast<double>(value) / 1.0e9;
    }
}
