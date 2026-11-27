#include "simulation/runtime/pacing_clock.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ssp4sim::runtime
{
    namespace
    {
        int64_t steady_now_ns()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    PacingClock::PacingClock(double initial_speed, uint64_t start_time_ns)
    {
        if (!std::isfinite(initial_speed) || initial_speed < 0.0)
        {
            throw std::invalid_argument(
                "PacingClock: initial speed must be finite and non-negative");
        }

        speed_ = initial_speed;
        anchor_sim_ns_ = start_time_ns;
        anchor_wall_ns_ = steady_now_ns();
    }

    double PacingClock::speed() const
    {
        return speed_;
    }

    int64_t PacingClock::anchor_wall_ns() const
    {
        return anchor_wall_ns_;
    }

    uint64_t PacingClock::anchor_sim_ns() const
    {
        return anchor_sim_ns_;
    }

    void PacingClock::apply_speed(double speed, uint64_t sim_time_ns)
    {
        speed_ = speed;
        anchor_at(sim_time_ns);
    }

    void PacingClock::anchor_at(uint64_t sim_time_ns)
    {
        anchor_wall_ns_ = steady_now_ns();
        anchor_sim_ns_ = sim_time_ns;
    }

    uint64_t PacingClock::deadline_ns(int64_t anchor_wall_ns,
                                      uint64_t anchor_sim_ns,
                                      uint64_t sim_time_ns,
                                      double speed)
    {
        if (!(speed > 0.0) || !std::isfinite(speed))
        {
            return 0;
        }

        if (sim_time_ns <= anchor_sim_ns)
        {
            return anchor_wall_ns > 0
                       ? static_cast<uint64_t>(anchor_wall_ns)
                       : 0;
        }

        const long double elapsed_sim_ns =
            static_cast<long double>(sim_time_ns - anchor_sim_ns);
        const long double wall_offset_ns =
            elapsed_sim_ns / static_cast<long double>(speed);
        const long double deadline_ns =
            static_cast<long double>(anchor_wall_ns) + wall_offset_ns;

        if (deadline_ns <= 0.0L)
        {
            return 0;
        }

        constexpr long double max_deadline =
            static_cast<long double>(std::numeric_limits<uint64_t>::max());

        if (deadline_ns >= max_deadline)
        {
            return std::numeric_limits<uint64_t>::max();
        }

        return static_cast<uint64_t>(deadline_ns + 0.5L);
    }
}
