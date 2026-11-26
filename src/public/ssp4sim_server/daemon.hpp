#pragma once

#include "protocol.hpp"
#include "simulation/runner/threaded_runner.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ssp4simd
{
    /// Owns the single ThreadedRunner instance. Every entry point is called from
    /// the server's poll thread, so no locking is needed. Events are pulled by
    /// the server through collect_events().
    class Daemon
    {
    public:
        explicit Daemon(std::string config_path);
        ~Daemon();

        Daemon(const Daemon &) = delete;
        Daemon &operator=(const Daemon &) = delete;
        Daemon(Daemon &&) noexcept = default;
        Daemon &operator=(Daemon &&) noexcept = default;

        /// Handle one request and return the response to send.
        json handle(const json &request);

        /// Stop a live run (if any) and wait for it to finish.
        void shutdown();

        /// Perform one monitor iteration. The caller schedules it every 50 ms.
        void monitor_tick();

        /// Events produced since the previous call.
        std::vector<json> collect_events();

    private:
        json do_state(const json &id);
        json do_reset(const json &id);

        void stop_and_wait();
        void stop_and_emit_terminal();
        void emit(json event);
        void reset_monitor_baseline(const ssp4sim::RunInfo &info);

        std::string config_path_;

        std::unique_ptr<ssp4sim::ThreadedRunner> runner_;

        std::vector<json> events_;

        // Monitor bookkeeping, touched only by monitor_tick() and do_reset().
        std::chrono::steady_clock::time_point last_time_emit_{};
        uint64_t last_time_sim_ns_ = 0;
        ssp4sim::RunState observed_state_ = ssp4sim::RunState::Created;
    };
}
