#include "daemon.hpp"

#include "runtime_control.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
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

        json artifacts_json(const std::vector<ssp4sim::RecordedArtifact> &artifacts)
        {
            json result = json::array();
            for (const auto &artifact : artifacts)
            {
                result.push_back(json{{"kind", artifact.kind},
                                      {"path", artifact.path},
                                      {"run_id", artifact.run_id}});
            }
            return result;
        }

        json state_event(const ssp4sim::RunInfo &info)
        {
            json event{{"v", 1},
                       {"event", "state"},
                       {"state", state_to_string(info.state)},
                       {"sim_time", ns_to_seconds(info.sim_time_ns)},
                       {"speed", info.speed}};
            event["session_id"] = info.session_id;
            event["artifacts"] = artifacts_json(info.artifacts);
            return event;
        }

        json time_event(const ssp4sim::RunInfo &info)
        {
            return json{{"v", 1},
                        {"event", "time"},
                        {"sim_time", ns_to_seconds(info.sim_time_ns)},
                        {"speed", info.speed}};
        }

        json done_event(ssp4sim::RunResult result,
                        const ssp4sim::RunInfo &info)
        {
            json event{{"v", 1},
                       {"event", "done"},
                       {"result", result_to_string(result)},
                       {"sim_time", ns_to_seconds(info.sim_time_ns)}};
            event["session_id"] = info.session_id;
            event["artifacts"] = artifacts_json(info.artifacts);
            return event;
        }

        json error_event(const std::string &code, const std::string &message)
        {
            return json{{"v", 1},
                        {"event", "error"},
                        {"code", code},
                        {"message", message}};
        }

        /// True when \p candidate resolves to a path at or under \p root. Both
        /// paths are canonicalized first so `..` and symlinks cannot escape the
        /// policy root.
        bool path_is_within(const std::filesystem::path &root,
                            const std::filesystem::path &candidate)
        {
            std::error_code error;

            // Resolve a final symlink explicitly: weakly_canonical leaves a
            // dangling symlink unresolved, but opening the path would still
            // follow it to its target.
            std::filesystem::path resolved = candidate;
            if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(candidate, error)))
            {
                error.clear();
                const auto target =
                    std::filesystem::read_symlink(candidate, error);
                if (error)
                {
                    return false;
                }
                resolved = target.is_absolute()
                               ? target
                               : candidate.parent_path() / target;
            }

            const auto canonical_root =
                std::filesystem::weakly_canonical(root, error);
            if (error)
            {
                return false;
            }
            const auto canonical_candidate =
                std::filesystem::weakly_canonical(resolved, error);
            if (error)
            {
                return false;
            }

            auto root_part = canonical_root.begin();
            auto candidate_part = canonical_candidate.begin();
            for (; root_part != canonical_root.end();
                 ++root_part, ++candidate_part)
            {
                if (candidate_part == canonical_candidate.end() ||
                    *root_part != *candidate_part)
                {
                    return false;
                }
            }
            return true;
        }

        /// Single place where engine exceptions become protocol error codes.
        json exception_to_error(const json &request, const std::exception &error)
        {
            const auto id = request_id(request);
            if (const auto *override_error =
                    dynamic_cast<const ssp4sim::OverrideError *>(&error);
                override_error != nullptr)
            {
                return make_error(id, override_error->code, error.what());
            }
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
    {
        auto config = ssp4sim::SharedConfig::from_file(config_path);
        if (config.snapshot_dir)
        {
            snapshot_dir_ =
                std::filesystem::weakly_canonical(*config.snapshot_dir);
        }

        runner_ = std::make_unique<ssp4sim::Simulator>(std::move(config));
        runner_->init();
        const auto info = runner_->info();
        reset_monitor_baseline(info);
    }

    Daemon::Daemon(Daemon &&other) noexcept
        : snapshot_dir_(std::move(other.snapshot_dir_)),
          runner_(std::move(other.runner_)),
          snapshot_requests_(std::move(other.snapshot_requests_)),
          snapshot_completions_(std::move(other.snapshot_completions_)),
          snapshot_thread_(std::move(other.snapshot_thread_)),
          snapshot_stopping_(other.snapshot_stopping_),
          lifecycle_thread_(std::move(other.lifecycle_thread_)),
          lifecycle_ok_(other.lifecycle_ok_),
          lifecycle_error_(std::move(other.lifecycle_error_)),
          lifecycle_ready_(other.lifecycle_ready_),
          lifecycle_had_run_(other.lifecycle_had_run_),
          lifecycle_old_info_(other.lifecycle_old_info_),
          lifecycle_old_error_(std::move(other.lifecycle_old_error_)),
          lifecycle_busy_(other.lifecycle_busy_),
          lifecycle_operation_(other.lifecycle_operation_),
          lifecycle_start_(other.lifecycle_start_),
          lifecycle_cached_info_(other.lifecycle_cached_info_),
          events_(std::move(other.events_)),
          override_events_(std::move(other.override_events_)),
          next_override_seq_(other.next_override_seq_),
          last_time_emit_(other.last_time_emit_),
          last_time_sim_ns_(other.last_time_sim_ns_),
          observed_state_(other.observed_state_)
    {
        other.lifecycle_busy_ = false;
        other.lifecycle_operation_ = LifecycleOperation::None;
    }

    Daemon::~Daemon()
    {
        shutdown();
    }

    void Daemon::shutdown()
    {
        // A lifecycle operation owns the runner until its thread finishes.
        if (lifecycle_thread_.joinable())
        {
            lifecycle_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            lifecycle_ready_ = false;
            lifecycle_ok_ = false;
            lifecycle_error_.clear();
            lifecycle_had_run_ = false;
            lifecycle_old_info_ = {};
            lifecycle_old_error_.clear();
        }
        lifecycle_busy_ = false;
        lifecycle_operation_ = LifecycleOperation::None;
        lifecycle_start_ = false;

        clear_override_timeline();

        // Take the snapshot worker down first so it cannot observe the runner
        // while shutdown tears it down.
        stop_snapshot_worker();
        stop_and_wait();
        monitor_tick();
    }

    void Daemon::stop_and_wait()
    {
        if (!runner_)
        {
            return;
        }

        // A failed run rethrows from wait(); reset/shutdown discard it.
        runner_->shutdown();
    }

    json Daemon::handle(const json &request)
    {
        try
        {
            const auto id = request_id(request);
            const auto command = request["cmd"].get<std::string>();

            // A lifecycle operation owns the runner until the poll thread
            // takes the handoff. Only observation and teardown remain safe
            // while it runs.
            if (lifecycle_busy_ && command != "state" && command != "shutdown")
            {
                return make_error(id, "server_busy",
                                  "a lifecycle operation is in progress");
            }

            if (command == "state")
            {
                return do_state(id);
            }
            if (command == "start")
            {
                runner_->start();
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
                runner_->pause(at_ns);
                return make_ok(id);
            }
            if (command == "resume")
            {
                runner_->resume();
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
                runner_->stop(at_ns);
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
                runner_->set_speed(speed, at_ns);
                return make_ok(id);
            }
            if (command == "set_override")
            {
                return do_set_override(id, request, false);
            }
            if (command == "clear_override")
            {
                return do_set_override(id, request, true);
            }
            if (command == "list_overrides")
            {
                return do_list_overrides(id);
            }
            if (command == "reset")
            {
                return do_reset(id);
            }
            if (command == "snapshot")
            {
                if (!request.contains("path") || !request["path"].is_string() ||
                    request["path"].get<std::string>().empty())
                {
                    return make_error(id, "bad_value",
                                      "\"path\" must be a non-empty string");
                }
                const std::filesystem::path path(
                    request["path"].get<std::string>());
                if (snapshot_dir_ && !path_is_within(*snapshot_dir_, path))
                {
                    return make_error(
                        id, "bad_value",
                        "\"path\" must be inside the configured snapshot "
                        "directory: " +
                            snapshot_dir_->string());
                }
                const ssp4sim::RunState state = runner_->info().state;
                if (state != ssp4sim::RunState::Running &&
                    state != ssp4sim::RunState::Paused)
                {
                    throw std::logic_error(
                        "Simulator::snapshot() requires a Running or Paused "
                        "simulation");
                }

                {
                    std::lock_guard<std::mutex> lock(snapshot_mutex_);
                    snapshot_requests_.push_back(
                        SnapshotRequest{id.is_null() ? std::string{}
                                                    : id.dump(),
                                        path});
                }
                ensure_snapshot_worker();
                snapshot_cv_.notify_one();

                json response = make_ok(id);
                response["accepted"] = true;
                return response;
            }
            if (command == "restore")
            {
                return do_restore(id, request);
            }
            if (command == "shutdown")
            {
                shutdown();
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
        const auto info =
            lifecycle_busy_ ? lifecycle_cached_info_ : runner_->info();
        json response = make_ok(id);
        response["state"] = state_to_string(info.state);
        response["sim_time"] = ns_to_seconds(info.sim_time_ns);
        response["speed"] = info.speed;
        response["session_id"] = info.session_id;
        response["artifacts"] = artifacts_json(info.artifacts);
        return response;
    }

    json Daemon::do_reset(const json &id)
    {
        if (!begin_lifecycle(LifecycleOperation::Reset, {}, false))
        {
            return make_error(id, "server_busy",
                              "a lifecycle operation is in progress");
        }

        json response = make_ok(id);
        response["accepted"] = true;
        return response;
    }

    json Daemon::do_restore(const json &id, const json &request)
    {
        if (!request.contains("path") || !request["path"].is_string() ||
            request["path"].get<std::string>().empty())
        {
            return make_error(id, "bad_value",
                              "\"path\" must be a non-empty string");
        }
        const std::filesystem::path snapshot_path(
            request["path"].get<std::string>());
        if (snapshot_dir_ && !path_is_within(*snapshot_dir_, snapshot_path))
        {
            return make_error(id, "bad_value",
                              "\"path\" must be inside the configured snapshot "
                              "directory: " +
                                  snapshot_dir_->string());
        }

        bool start = true;
        if (request.contains("start") && !request["start"].is_null())
        {
            if (!request["start"].is_boolean())
            {
                return make_error(id, "bad_value",
                                  "\"start\" must be a boolean");
            }
            start = request["start"].get<bool>();
        }

        if (!begin_lifecycle(LifecycleOperation::Restore, snapshot_path, start))
        {
            return make_error(id, "server_busy",
                              "a lifecycle operation is in progress");
        }

        json response = make_ok(id);
        response["accepted"] = true;
        return response;
    }

    json Daemon::do_set_override(const json &id, const json &request,
                                 bool clear)
    {
        ssp4sim::VariableSide side;
        if (!get_side_field(request, side))
        {
            return make_error(id, "bad_value",
                              "\"side\" must be \"input\" or \"output\"");
        }

        std::string model;
        if (!get_string_field(request, "model", model))
        {
            return make_error(id, "bad_value",
                              "\"model\" must be a non-empty string");
        }

        std::string name;
        if (!get_string_field(request, "name", name))
        {
            return make_error(id, "bad_value",
                              "\"name\" must be a non-empty string");
        }

        std::optional<uint64_t> at_ns;
        std::string at_error;
        if (!get_at_ns_field(request, at_ns, at_error))
        {
            return make_error(id, "bad_value", at_error);
        }

        double k = 0.0;
        double m = 0.0;
        if (!clear)
        {
            if (!get_number_field(request, "k", k) ||
                !get_number_field(request, "m", m))
            {
                return make_error(id, "bad_value",
                                  "\"k\" and \"m\" must be finite numbers");
            }
        }

        if (clear)
        {
            runner_->clear_override(side, model, name, at_ns);
        }
        else
        {
            runner_->set_override(side, model, name, k, m, at_ns);
        }

        OverrideEvent event;
        event.side = side;
        event.model = std::move(model);
        event.name = std::move(name);
        event.k = k;
        event.m = m;
        event.set = !clear;
        event.at_ns = at_ns.value_or(0);
        override_events_.emplace(
            std::make_pair(event.at_ns, next_override_seq_++),
            std::move(event));

        return make_ok(id);
    }

    json Daemon::do_list_overrides(const json &id)
    {
        const auto info = runner_->info();
        const uint64_t now = info.sim_time_ns;

        std::map<std::tuple<ssp4sim::VariableSide, std::string, std::string>,
                 const OverrideEvent *>
            active;
        for (const auto &[key, event] : override_events_)
        {
            (void)key;
            if (event.at_ns != 0 && event.at_ns > now)
            {
                break;
            }

            const auto target =
                std::make_tuple(event.side, event.model, event.name);
            if (event.set)
            {
                active[target] = &event;
            }
            else
            {
                active.erase(target);
            }
        }

        json overrides = json::array();
        for (const auto &[target, event] : active)
        {
            (void)target;
            overrides.push_back(json{{"side", ssp4sim::to_string(event->side)},
                                     {"model", event->model},
                                     {"name", event->name},
                                     {"k", event->k},
                                     {"m", event->m}});
        }

        json response = make_ok(id);
        response["overrides"] = std::move(overrides);
        return response;
    }

    void Daemon::clear_override_timeline()
    {
        override_events_.clear();
    }

    bool Daemon::begin_lifecycle(LifecycleOperation operation,
                                 const std::filesystem::path &snapshot,
                                 bool start)
    {
        if (lifecycle_busy_)
        {
            return false;
        }

        // The previous background thread has published its result and is on
        // its way out once lifecycle_busy_ is clear; reap it before reusing
        // the member.
        if (lifecycle_thread_.joinable())
        {
            lifecycle_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            lifecycle_ready_ = false;
            lifecycle_ok_ = false;
            lifecycle_error_.clear();
            lifecycle_had_run_ = false;
            lifecycle_old_info_ = {};
            lifecycle_old_error_.clear();
        }

        // Reset/restore rebuild the session and discard control state.
        clear_override_timeline();

        // The lifecycle thread becomes the only user of the runner, so the
        // snapshot worker must be gone first; its pending requests settle as
        // run_ended.
        stop_snapshot_worker();
        lifecycle_cached_info_ = runner_->info();

        lifecycle_busy_ = true;
        lifecycle_operation_ = operation;
        lifecycle_start_ = start;
        lifecycle_thread_ =
            std::thread(&Daemon::run_lifecycle, this, operation,
                        std::filesystem::path(snapshot));
        return true;
    }

    void Daemon::run_lifecycle(LifecycleOperation operation,
                               std::filesystem::path snapshot)
    {
        // Uses the public lifecycle API on the one runner. The poll thread
        // does not touch the runner until lifecycle_ready_ is published.
        const auto before = runner_->info();
        const bool had_run =
            before.state != ssp4sim::RunState::Created &&
            before.state != ssp4sim::RunState::Initialized;

        std::string old_error;
        if (had_run)
        {
            runner_->shutdown();
            try
            {
                (void)runner_->wait();
            }
            catch (const std::exception &error)
            {
                old_error = error.what();
            }
            catch (...)
            {
                old_error = "unknown error";
            }
        }

        const auto old_info = runner_->info();

        bool ok = false;
        std::string message;
        try
        {
            if (operation == LifecycleOperation::Restore)
            {
                runner_->restore(snapshot, false);
            }
            else
            {
                runner_->reset();
            }
            ok = true;
        }
        catch (const std::exception &error)
        {
            message = error.what();
        }
        catch (...)
        {
            message = "unknown error";
        }

        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lifecycle_ok_ = ok;
        lifecycle_error_ = std::move(message);
        lifecycle_had_run_ = had_run;
        lifecycle_old_info_ = old_info;
        lifecycle_old_error_ = std::move(old_error);
        lifecycle_ready_ = true;
    }

    json Daemon::lifecycle_event(LifecycleOperation operation, bool ok,
                                 const std::string &error,
                                 const ssp4sim::RunInfo &info)
    {
        const char *name =
            operation == LifecycleOperation::Restore ? "restore" : "reset";
        json event = json{{"v", 1},
                          {"event", "lifecycle"},
                          {"operation", name},
                          {"ok", ok}};
        if (!ok)
        {
            event["code"] = operation == LifecycleOperation::Restore
                                ? "restore_failed"
                                : "reset_failed";
            event["error"] = error.empty() ? "unknown error" : error;
        }
        else
        {
            event["state"] = state_to_string(info.state);
            event["sim_time"] = ns_to_seconds(info.sim_time_ns);
        }
        return event;
    }

    void Daemon::emit(json event)
    {
        events_.push_back(std::move(event));
    }

    void Daemon::emit_snapshot_event(const SnapshotCompletion &completion)
    {
        const std::string code =
            completion.ok ? "ok"
                          : (completion.code.empty() ? "snapshot_failed"
                                                     : completion.code);
        json event = json{{"v", 1},
                          {"event", "snapshot"},
                          {"ok", completion.ok},
                          {"code", code},
                          {"path", completion.path.string()}};
        if (!completion.id.empty())
        {
            json id;
            try
            {
                id = json::parse(completion.id);
            }
            catch (const json::exception &)
            {
                id = completion.id;
            }
            event["id"] = std::move(id);
        }
        if (completion.ok)
        {
            std::error_code error;
            const auto bytes =
                std::filesystem::file_size(completion.path, error);
            if (!error)
            {
                event["bytes"] = bytes;
            }
        }
        else
        {
            event["error"] = completion.message;
        }
        emit(std::move(event));
    }

    void Daemon::drain_snapshot_completions()
    {
        std::deque<SnapshotCompletion> completions;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            completions.swap(snapshot_completions_);
        }
        for (const SnapshotCompletion &completion : completions)
        {
            emit_snapshot_event(completion);
        }
    }

    void Daemon::ensure_snapshot_worker()
    {
        // Only the poll thread calls this; a joinable worker is already alive
        // and waiting for its next request.
        if (snapshot_thread_.joinable())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_stopping_ = false;
        }
        snapshot_thread_ = std::thread(&Daemon::run_snapshot_worker, this);
    }

    void Daemon::stop_snapshot_worker()
    {
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_stopping_ = true;
        }
        snapshot_cv_.notify_all();

        if (snapshot_thread_.joinable())
        {
            snapshot_thread_.join();
        }

        std::deque<SnapshotRequest> pending;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            pending.swap(snapshot_requests_);
            snapshot_stopping_ = false;
        }

        for (const SnapshotRequest &request : pending)
        {
            SnapshotCompletion completion;
            completion.id = request.id;
            completion.path = request.path;
            completion.ok = false;
            completion.code = "run_ended";
            completion.message = "run ended before snapshot";
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_completions_.push_back(std::move(completion));
        }
    }

    void Daemon::run_snapshot_worker()
    {
        for (;;)
        {
            SnapshotRequest request;
            {
                std::unique_lock<std::mutex> lock(snapshot_mutex_);
                snapshot_cv_.wait(
                    lock, [this]()
                    { return snapshot_stopping_ || !snapshot_requests_.empty(); });

                if (snapshot_stopping_)
                {
                    return;
                }

                request = std::move(snapshot_requests_.front());
                snapshot_requests_.pop_front();
            }

            SnapshotCompletion completion;
            completion.id = request.id;
            completion.path = request.path;

            try
            {
                completion.ok = runner_->snapshot(request.path);
                if (!completion.ok)
                {
                    const ssp4sim::RunState state = runner_->info().state;
                    if (state != ssp4sim::RunState::Running &&
                        state != ssp4sim::RunState::Paused)
                    {
                        completion.code = "run_ended";
                        completion.message = "run ended before snapshot";
                    }
                    else
                    {
                        completion.code = "snapshot_failed";
                        completion.message = "snapshot failed";
                    }
                }
            }
            catch (const std::exception &error)
            {
                completion.code = "snapshot_failed";
                completion.message = error.what();
            }
            catch (...)
            {
                completion.code = "snapshot_failed";
                completion.message = "unknown error";
            }

            {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_completions_.push_back(std::move(completion));
            }
            snapshot_cv_.notify_all();
        }
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
        last_time_emit_ = std::chrono::steady_clock::now();
        last_time_sim_ns_ = info.sim_time_ns;
    }

    void Daemon::monitor_tick()
    {
        // Completed snapshot work is emitted regardless of runner state.
        drain_snapshot_completions();

        if (!runner_)
        {
            return;
        }

        using clock = std::chrono::steady_clock;
        const auto now = clock::now();

        if (lifecycle_busy_)
        {
            bool ok = false;
            std::string error;
            bool had_run = false;
            ssp4sim::RunInfo old_info{};
            std::string old_error;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                if (!lifecycle_ready_)
                {
                    return;
                }
                ok = lifecycle_ok_;
                error = lifecycle_error_;
                had_run = lifecycle_had_run_;
                old_info = lifecycle_old_info_;
                old_error = lifecycle_old_error_;
                lifecycle_ready_ = false;
                lifecycle_ok_ = false;
                lifecycle_error_.clear();
            }

            const LifecycleOperation operation = lifecycle_operation_;
            const bool start = lifecycle_start_;
            lifecycle_busy_ = false;
            lifecycle_operation_ = LifecycleOperation::None;
            lifecycle_start_ = false;

            drain_snapshot_completions();

            const bool already_reported =
                ssp4sim::is_terminal(observed_state_) &&
                observed_state_ == old_info.state;
            if (had_run && ssp4sim::is_terminal(old_info.state) &&
                !already_reported)
            {
                if (old_info.state != observed_state_)
                {
                    emit(state_event(old_info));
                }
                if (old_info.state == ssp4sim::RunState::Failed &&
                    !old_error.empty())
                {
                    emit(error_event("simulation_failed", old_error));
                }
                emit(done_event(ssp4sim::terminal_result(old_info.state),
                                old_info));
            }

            // The old terminal must not be reported again by the generic path.
            observed_state_ = old_info.state;
            last_time_emit_ = now;
            last_time_sim_ns_ = old_info.sim_time_ns;

            if (!ok)
            {
                emit(lifecycle_event(operation, false, error, old_info));
            }
            else
            {
                const auto info = runner_->info();
                reset_monitor_baseline(info);
                emit(state_event(info));

                std::string start_error;
                if (start)
                {
                    try
                    {
                        runner_->start();
                    }
                    catch (const std::exception &exception)
                    {
                        start_error = exception.what();
                    }
                    catch (...)
                    {
                        start_error = "unknown error";
                    }
                }

                if (start_error.empty())
                {
                    emit(lifecycle_event(operation, true, {},
                                         runner_->info()));
                }
                else
                {
                    emit(lifecycle_event(operation, false, start_error,
                                         runner_->info()));
                }
            }

            // The per-job result has been consumed; clear it so a later tick
            // cannot report stale old-run data.
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                lifecycle_had_run_ = false;
                lifecycle_old_info_ = {};
                lifecycle_old_error_.clear();
            }
        }

        const auto info = runner_->info();
        const auto state = info.state;
        const bool state_changed = state != observed_state_;

        if (state_changed)
        {
            observed_state_ = state;
            emit(state_event(info));
            last_time_emit_ = now;
            last_time_sim_ns_ = info.sim_time_ns;

            if (!lifecycle_busy_ && ssp4sim::is_terminal(state))
            {
                clear_override_timeline();

                std::string message;
                try
                {
                    runner_->wait();
                }
                catch (const std::exception &error)
                {
                    message = error.what();
                }
                catch (...)
                {
                    message = "simulation failed with an unknown error";
                }

                if (state == ssp4sim::RunState::Failed)
                {
                    emit(error_event("simulation_failed",
                                     message.empty() ? "simulation failed"
                                                     : message));
                }

                emit(done_event(ssp4sim::terminal_result(state), info));
            }
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
    }
}
