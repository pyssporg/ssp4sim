#include "daemon.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssp4simd
{
    namespace
    {
        constexpr auto time_event_period = std::chrono::milliseconds(100);

        json request_id(const json &request)
        {
            return request.contains("id") ? request["id"] : json(nullptr);
        }

        bool get_number_field(const json &request, const char *name, double &out)
        {
            if (!request.contains(name) || !request[name].is_number())
            {
                return false;
            }
            out = request[name].get<double>();
            return std::isfinite(out);
        }

        /// Parse the optional "at" field (simulation seconds) into nanoseconds.
        /// Absent or null leaves `out` empty. Returns false with `message` set
        /// when "at" is present but not a finite number >= 0 or does not fit in
        /// uint64_t nanoseconds.
        bool get_at_ns_field(const json &request, std::optional<uint64_t> &out,
                             std::string &message)
        {
            out.reset();
            if (!request.contains("at") || request["at"].is_null())
            {
                return true;
            }

            double at_seconds = 0.0;
            if (!get_number_field(request, "at", at_seconds) ||
                at_seconds < 0.0)
            {
                message = "\"at\" must be a finite number >= 0";
                return false;
            }

            const long double ns =
                static_cast<long double>(at_seconds) * 1.0e9L;
            if (ns >
                static_cast<long double>(std::numeric_limits<uint64_t>::max()))
            {
                message = "\"at\" is too large";
                return false;
            }
            out = static_cast<uint64_t>(ns);
            return true;
        }

        json state_event(const ssp4sim::RunInfo &info)
        {
            return json{{"v", 1},
                        {"event", "state"},
                        {"state", state_to_string(info.state)},
                        {"sim_time", ns_to_seconds(info.sim_time_ns)},
                        {"speed", info.speed}};
        }

        json time_event(const ssp4sim::RunInfo &info)
        {
            return json{{"v", 1},
                        {"event", "time"},
                        {"sim_time", ns_to_seconds(info.sim_time_ns)},
                        {"speed", info.speed}};
        }

        json done_event(ssp4sim::RunResult result, uint64_t sim_time_ns)
        {
            return json{{"v", 1},
                        {"event", "done"},
                        {"result", result_to_string(result)},
                        {"sim_time", ns_to_seconds(sim_time_ns)}};
        }

        json error_event(const std::string &code, const std::string &message)
        {
            return json{{"v", 1},
                        {"event", "error"},
                        {"code", code},
                        {"message", message}};
        }

        /// Single place where engine exceptions become protocol error codes.
        json exception_to_error(const json &request, const std::exception &error)
        {
            const auto id = request_id(request);
            if (dynamic_cast<const std::invalid_argument *>(&error) != nullptr)
            {
                return make_error(id, "bad_value", error.what());
            }
            if (dynamic_cast<const std::logic_error *>(&error) != nullptr)
            {
                return make_error(id, "bad_state", error.what());
            }
            return make_error(id, "engine_error", error.what());
        }
    }

    Daemon::Daemon(std::string config_path)
        : config_path_(std::move(config_path))
    {
        sim_ = std::make_unique<ssp4sim::Simulator>(config_path_);
        sim_->init();
        const auto info = sim_->info();
        reset_monitor_baseline(info);
        emit(state_event(info));
    }

    Daemon::~Daemon()
    {
        shutdown();
    }

    void Daemon::shutdown()
    {
        stop_and_wait();
    }

    void Daemon::stop_and_wait()
    {
        if (!sim_)
        {
            return;
        }

        const auto info = sim_->info();

        // Pre-run: there is no run thread to stop or join.
        if (info.state == ssp4sim::RunState::Created ||
            info.state == ssp4sim::RunState::Initialized)
        {
            return;
        }

        // Request stop for every non-terminal state. stop() is idempotent once
        // Stopping or terminal, so it also covers a just-started run.
        if (!ssp4sim::is_terminal(info.state))
        {
            try
            {
                sim_->stop();
            }
            catch (const std::exception &)
            {
                // The run may have reached a terminal state between the check
                // and the call; wait() below still joins it.
            }
        }

        if (run_started_)
        {
            try
            {
                sim_->wait();
            }
            catch (const std::exception &)
            {
                // A failed run rethrows from wait(); reset/shutdown discard it.
            }
            run_started_ = false;
        }
    }

    json Daemon::handle(const json &request)
    {
        try
        {
            const auto id = request_id(request);
            const auto command = request["cmd"].get<std::string>();

            if (command == "state")
            {
                return do_state(id);
            }
            if (command == "start")
            {
                sim_->start();
                run_started_ = true;
                return make_ok(id);
            }
            if (command == "pause")
            {
                std::optional<uint64_t> at_ns;
                std::string at_error;
                if (!get_at_ns_field(request, at_ns, at_error))
                {
                    return make_error(id, "bad_value", at_error);
                }
                sim_->pause(at_ns);
                return make_ok(id);
            }
            if (command == "resume")
            {
                sim_->resume();
                return make_ok(id);
            }
            if (command == "stop")
            {
                std::optional<uint64_t> at_ns;
                std::string at_error;
                if (!get_at_ns_field(request, at_ns, at_error))
                {
                    return make_error(id, "bad_value", at_error);
                }
                sim_->stop(at_ns);
                return make_ok(id);
            }
            if (command == "set_speed")
            {
                double speed = 0.0;
                if (!get_number_field(request, "speed", speed))
                {
                    return make_error(id, "bad_speed",
                                      "\"speed\" must be a finite number");
                }
                if (speed < 0.0)
                {
                    return make_error(id, "bad_speed",
                                      "\"speed\" must be >= 0");
                }
                std::optional<uint64_t> at_ns;
                std::string at_error;
                if (!get_at_ns_field(request, at_ns, at_error))
                {
                    return make_error(id, "bad_value", at_error);
                }
                sim_->set_speed(speed, at_ns);
                return make_ok(id);
            }
            if (command == "reset")
            {
                return do_reset(id);
            }
            if (command == "shutdown")
            {
                stop_and_wait();
                return make_ok(id);
            }

            return make_error(id, "unknown_command",
                              "unknown command \"" + command + "\"");
        }
        catch (const std::exception &error)
        {
            return exception_to_error(request, error);
        }
        catch (...)
        {
            return make_error(request_id(request), "internal_error",
                              "unknown internal error");
        }
    }

    json Daemon::do_state(const json &id)
    {
        const auto info = sim_->info();
        json response = make_ok(id);
        response["state"] = state_to_string(info.state);
        response["sim_time"] = ns_to_seconds(info.sim_time_ns);
        response["speed"] = info.speed;
        return response;
    }

    json Daemon::do_reset(const json &id)
    {
        stop_and_wait();

        std::unique_ptr<ssp4sim::Simulator> fresh;
        try
        {
            fresh = std::make_unique<ssp4sim::Simulator>(config_path_);
            fresh->init();
        }
        catch (const std::exception &error)
        {
            return make_error(id, "reset_failed", error.what());
        }

        sim_ = std::move(fresh);
        run_started_ = false;
        const auto info = sim_->info();
        reset_monitor_baseline(info);
        emit(state_event(info));
        return make_ok(id);
    }

    void Daemon::emit(const json &event)
    {
        events_.push_back(event);
    }

    std::vector<json> Daemon::collect_events()
    {
        std::vector<json> events;
        events.swap(events_);
        return events;
    }

    void Daemon::reset_monitor_baseline(const ssp4sim::RunInfo &info)
    {
        observed_state_ = info.state;
        terminal_reported_ = false;
        last_time_emit_ = std::chrono::steady_clock::now();
        last_time_sim_ns_ = info.sim_time_ns;
    }

    void Daemon::monitor_tick()
    {
        if (!sim_)
        {
            return;
        }

        using clock = std::chrono::steady_clock;

        const auto now = clock::now();
        const auto info = sim_->info();
        const auto state = info.state;

        if (state != observed_state_)
        {
            observed_state_ = state;
            emit(state_event(info));
            last_time_emit_ = now;
            last_time_sim_ns_ = info.sim_time_ns;
        }
        else if (state == ssp4sim::RunState::Running ||
                 state == ssp4sim::RunState::Paused)
        {
            if (now - last_time_emit_ >= time_event_period &&
                info.sim_time_ns != last_time_sim_ns_)
            {
                emit(time_event(info));
                last_time_emit_ = now;
                last_time_sim_ns_ = info.sim_time_ns;
            }
        }

        if (ssp4sim::is_terminal(state) && !terminal_reported_)
        {
            terminal_reported_ = true;

            ssp4sim::RunResult result = ssp4sim::RunResult::Failed;
            if (state == ssp4sim::RunState::Failed)
            {
                std::string message = "simulation failed";
                try
                {
                    sim_->wait();
                }
                catch (const std::exception &error)
                {
                    message = error.what();
                }
                catch (...)
                {
                    message = "simulation failed with an unknown error";
                }
                emit(error_event("simulation_failed", message));
            }
            else
            {
                try
                {
                    result = sim_->wait();
                }
                catch (const std::exception &)
                {
                    result = ssp4sim::RunResult::Failed;
                }
                catch (...)
                {
                    result = ssp4sim::RunResult::Failed;
                }
            }

            run_started_ = false;
            emit(done_event(result, info.sim_time_ns));
        }
    }
}
