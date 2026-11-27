#pragma once

#include <cstdint>

namespace ssp4sim::runtime
{
    /// Unlocked pacing: speed and wall/simulation anchors. It owns no lock and
    /// no simulation time; SimulatorCore::sim_time_ns() is the single source.
    class PacingClock
    {
    public:
        PacingClock(double initial_speed, uint64_t start_time_ns);

        double speed() const;
        int64_t anchor_wall_ns() const;
        uint64_t anchor_sim_ns() const;

        /// Set the speed and reanchor at \p sim_time_ns.
        void apply_speed(double speed, uint64_t sim_time_ns);

        /// Anchor wall time at now and simulation time at \p sim_time_ns.
        void anchor_at(uint64_t sim_time_ns);

        /// Wall-clock deadline for \p sim_time_ns, or 0 when no pacing applies.
        static uint64_t deadline_ns(int64_t anchor_wall_ns,
                                    uint64_t anchor_sim_ns,
                                    uint64_t sim_time_ns,
                                    double speed);

    private:
        double speed_ = 0.0;
        int64_t anchor_wall_ns_ = 0;
        uint64_t anchor_sim_ns_ = 0;
    };
}
