#pragma once

#include "protocol.hpp"
#include "cpp_api/simulator.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ssp4simd
{
    /// Owns the single Simulator as the async runtime. Every entry point is called from
    /// the server's poll thread. The only exception is a lifecycle operation,
    /// which drives the public Simulator API on lifecycle_thread_ and hands its
    /// result back to the poll thread through lifecycle_mutex_. Events are pulled
    /// by the server through collect_events().
    class Daemon
    {
    public:
        explicit Daemon(std::string config_path);
        ~Daemon();

        Daemon(const Daemon &) = delete;
        Daemon &operator=(const Daemon &) = delete;
        // A mutex and a thread prevent the implicit moves; construction still
        // moves a fresh daemon into the server, before any lifecycle work.
        Daemon(Daemon &&) noexcept;
        Daemon &operator=(Daemon &&) = delete;

        /// Handle one request and return the response to send.
        json handle(const json &request);

        /// Stop a live run (if any) and wait for it to finish.
        void shutdown();

        /// Perform one monitor iteration. The caller schedules it every 50 ms.
        void monitor_tick();

        /// Events produced since the previous call.
        std::vector<json> collect_events();

    private:
        enum class LifecycleOperation
        {
            None,
            Reset,
            Restore,
        };

        struct SnapshotRequest
        {
            std::string id;
            std::filesystem::path path;
        };

        struct SnapshotCompletion
        {
            std::string id;
            std::filesystem::path path;
            bool ok = false;
            std::string code;
            std::string message;
        };

        json do_state(const json &id);
        json do_reset(const json &id);
        json do_restore(const json &id, const json &request);

        bool begin_lifecycle(LifecycleOperation operation,
                             const std::filesystem::path &snapshot, bool start);
        void run_lifecycle(LifecycleOperation operation,
                           std::filesystem::path snapshot);
        json lifecycle_event(LifecycleOperation operation, bool ok,
                             const std::string &error,
                             const ssp4sim::RunInfo &info);

        void stop_and_wait();
        void emit(json event);
        void emit_snapshot_event(const SnapshotCompletion &completion);
        void drain_snapshot_completions();
        void ensure_snapshot_worker();
        void stop_snapshot_worker();
        void run_snapshot_worker();
        void reset_monitor_baseline(const ssp4sim::RunInfo &info);

        // The canonical checkpoint path policy (empty means any path is
        // allowed).
        std::optional<std::filesystem::path> snapshot_dir_;

        std::unique_ptr<ssp4sim::Simulator> runner_;

        // Daemon-owned async snapshot queue. The poll thread pushes requests
        // and drains completions; the worker is the only caller of
        // runner_->snapshot(). Started lazily, so moving a fresh daemon before
        // the first request never moves a running thread.
        std::mutex snapshot_mutex_;
        std::condition_variable snapshot_cv_;
        std::deque<SnapshotRequest> snapshot_requests_;
        std::deque<SnapshotCompletion> snapshot_completions_;
        std::thread snapshot_thread_;
        bool snapshot_stopping_ = false;

        // Lifecycle handoff. The background thread drives the public lifecycle
        // API on runner_ and publishes its result under lifecycle_mutex_; the
        // poll thread observes the runner only after it takes that result.
        std::thread lifecycle_thread_;
        std::mutex lifecycle_mutex_;
        bool lifecycle_ok_ = false;
        std::string lifecycle_error_;
        bool lifecycle_ready_ = false;
        bool lifecycle_had_run_ = false;
        ssp4sim::RunInfo lifecycle_old_info_{};
        std::string lifecycle_old_error_;

        // Poll-thread only.
        bool lifecycle_busy_ = false;
        LifecycleOperation lifecycle_operation_ = LifecycleOperation::None;
        bool lifecycle_start_ = false;
        ssp4sim::RunInfo lifecycle_cached_info_{};

        std::vector<json> events_;

        // Monitor bookkeeping, touched only by monitor_tick().
        std::chrono::steady_clock::time_point last_time_emit_{};
        uint64_t last_time_sim_ns_ = 0;
        ssp4sim::RunState observed_state_ = ssp4sim::RunState::Created;
    };
}
