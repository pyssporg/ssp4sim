#include <catch2/catch_test_macros.hpp>

#include "simulation/runtime/control_channel.hpp"
#include "simulation/runtime/pacing_clock.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace ssp4sim;
using namespace ssp4sim::runtime;

namespace
{
    /// Run post_and_wait() on a worker and expose its result.
    struct Submitter
    {
        Submitter(ControlChannel &channel, Action action)
            : done(promise.get_future())
        {
            thread = std::thread(
                [this, &channel, action]() mutable
                {
                    result = channel.post_and_wait(std::move(action));
                    promise.set_value();
                });
        }

        ~Submitter()
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        bool blocked(std::chrono::milliseconds timeout)
        {
            return done.wait_for(timeout) == std::future_status::timeout;
        }

        void wait_pending(const ControlChannel &channel)
        {
            for (int i = 0; i < 100'000 && channel.pending() == 0; ++i)
            {
                std::this_thread::yield();
            }
            REQUIRE(channel.pending() > 0);
        }

        std::promise<void> promise;
        std::shared_future<void> done;
        std::thread thread;
        AppliedAction result;
    };

    struct WorkWaiter
    {
        WorkWaiter(ControlChannel &channel, uint64_t sim_time_ns,
                   std::optional<uint64_t> deadline)
            : done(promise.get_future())
        {
            thread = std::thread(
                [this, &channel, sim_time_ns, deadline]()
                {
                    started.set_value();
                    channel.wait_for_work(sim_time_ns, deadline);
                    promise.set_value();
                });
            started.get_future().wait();
        }

        ~WorkWaiter()
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        bool blocked(std::chrono::milliseconds timeout)
        {
            return done.wait_for(timeout) == std::future_status::timeout;
        }

        std::promise<void> started;
        std::promise<void> promise;
        std::shared_future<void> done;
        std::thread thread;
    };
}

TEST_CASE("ControlChannel assigns sequences and exposes coherent status",
          "[control]")
{
    ControlChannel channel(2.0, 100);

    ChannelStatus status = channel.status();
    REQUIRE(status.state == RunState::Created);
    REQUIRE(status.sim_time_ns == 100);
    REQUIRE(status.speed == 2.0);
    REQUIRE(status.applied_seq == 0);
    REQUIRE_FALSE(status.error);

    Action start;
    start.kind = ActionKind::Start;
    REQUIRE(channel.post(start) == 1);

    Action pause;
    pause.kind = ActionKind::Pause;
    REQUIRE(channel.post(pause) == 2);
    REQUIRE(channel.status().applied_seq == 0);

    REQUIRE(channel.take_due(100).has_value());
    channel.settle(1, ActionStatus::Ok);
    REQUIRE(channel.status().applied_seq == 1);

    REQUIRE(channel.take_due(100).has_value());
    channel.settle(2, ActionStatus::Ok);
    REQUIRE(channel.status().applied_seq == 2);

    channel.set_state(RunState::Paused);
    channel.publish_time(250);
    channel.set_speed(4.0);
    status = channel.status();
    REQUIRE(status.state == RunState::Paused);
    REQUIRE(status.sim_time_ns == 250);
    REQUIRE(status.speed == 4.0);

    // Time is monotonic.
    channel.publish_time(150);
    REQUIRE(channel.status().sim_time_ns == 250);
}

TEST_CASE("ControlChannel orders by due time and keeps equal timestamps stable",
          "[control]")
{
    ControlChannel channel(0.0, 0);

    Action pause;
    pause.kind = ActionKind::Pause;
    pause.at_ns = 1000;
    REQUIRE(channel.post(pause) == 1);

    Action speed;
    speed.kind = ActionKind::SetSpeed;
    speed.at_ns = 500;
    speed.speed = 2.0;
    REQUIRE(channel.post(speed) == 2);

    Action stop;
    stop.kind = ActionKind::Stop;
    stop.at_ns = 1000;
    REQUIRE(channel.post(stop) == 3);

    Action resume;
    resume.kind = ActionKind::Resume;
    REQUIRE(channel.post(resume) == 4);

    auto due = channel.take_due(999);
    REQUIRE(due.has_value());
    REQUIRE(due->seq == 4); // due immediately, ahead of scheduled work
    channel.settle(due->seq, ActionStatus::Ok);

    due = channel.take_due(999);
    REQUIRE(due.has_value());
    REQUIRE(due->seq == 2);
    channel.settle(due->seq, ActionStatus::Ok);

    REQUIRE_FALSE(channel.take_due(999).has_value());

    due = channel.take_due(1000);
    REQUIRE(due.has_value());
    REQUIRE(due->seq == 1); // equal timestamps keep insertion order
    channel.settle(due->seq, ActionStatus::Ok);

    due = channel.take_due(1000);
    REQUIRE(due.has_value());
    REQUIRE(due->seq == 3);
    channel.settle(due->seq, ActionStatus::Ok);

    REQUIRE_FALSE(channel.take_due(2000).has_value());
    REQUIRE(channel.status().applied_seq == 4);
}

TEST_CASE("ControlChannel post_and_wait blocks until settled", "[control]")
{
    ControlChannel channel(0.0, 0);

    Action pause;
    pause.kind = ActionKind::Pause;
    Submitter submitter(channel, pause);
    submitter.wait_pending(channel);
    REQUIRE(submitter.blocked(std::chrono::milliseconds(0)));

    auto due = channel.take_due(0);
    REQUIRE(due.has_value());
    channel.settle(due->seq, ActionStatus::Ok);

    REQUIRE(submitter.done.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
    REQUIRE(submitter.result.status == ActionStatus::Ok);
}

TEST_CASE("ControlChannel close settles queued waiters Nok", "[control]")
{
    ControlChannel channel(0.0, 0);

    Action pause;
    pause.kind = ActionKind::Pause;
    Submitter submitter(channel, pause);
    submitter.wait_pending(channel);

    channel.close(std::make_exception_ptr(std::runtime_error("boom")));

    REQUIRE(submitter.done.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
    REQUIRE(submitter.result.status == ActionStatus::Nok);
    REQUIRE(submitter.result.error != nullptr);
    REQUIRE(channel.status().applied_seq >= 1);
    REQUIRE_FALSE(channel.take_due(0).has_value());
    REQUIRE(channel.is_closed());

    try
    {
        std::rethrow_exception(submitter.result.error);
    }
    catch (const std::runtime_error &error)
    {
        REQUIRE(std::string(error.what()) == "boom");
    }

    // A post after close is not queued; its waiter returns immediately.
    Action stop;
    stop.kind = ActionKind::Stop;
    REQUIRE(channel.post_and_wait(stop).status == ActionStatus::Nok);
    REQUIRE(channel.pending() == 0);
}

TEST_CASE("ControlChannel wait_for_work wakes on a due action", "[control]")
{
    ControlChannel channel(0.0, 0);
    channel.set_state(RunState::Paused);

    WorkWaiter waiter(channel, 0, std::nullopt);
    REQUIRE(waiter.blocked(std::chrono::milliseconds(0)));

    Action resume;
    resume.kind = ActionKind::Resume;
    (void)channel.post(resume);

    REQUIRE(waiter.done.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
}

TEST_CASE("ControlChannel wait_for_work wakes on a state change", "[control]")
{
    ControlChannel channel(0.0, 0);
    channel.set_state(RunState::Paused);

    WorkWaiter waiter(channel, 0, std::nullopt);
    REQUIRE(waiter.blocked(std::chrono::milliseconds(0)));

    channel.set_state(RunState::Stopping);

    REQUIRE(waiter.done.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
}

TEST_CASE("ControlChannel wait_for_work observes the wall deadline", "[control]")
{
    ControlChannel channel(0.0, 0);
    channel.set_state(RunState::Running);

    const auto deadline = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch())
                              .count() +
                          std::chrono::milliseconds(20).count();

    WorkWaiter waiter(channel, 0,
                      static_cast<uint64_t>(deadline));
    REQUIRE(waiter.done.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
}

TEST_CASE("ControlChannel wait_for_work returns immediately when closed",
          "[control]")
{
    ControlChannel channel(0.0, 0);
    channel.close(nullptr);

    channel.wait_for_work(0, std::nullopt);
}

TEST_CASE("PacingClock deadline math", "[control]")
{
    const int64_t anchor_wall = 1'000'000'000;

    CHECK(PacingClock::deadline_ns(anchor_wall, 0, 1'000'000'000, 1.0) ==
          2'000'000'000);
    CHECK(PacingClock::deadline_ns(anchor_wall, 0, 1'000'000'000, 2.0) ==
          1'500'000'000);
    CHECK(PacingClock::deadline_ns(anchor_wall, 0, 1'000'000'000, 0.5) ==
          3'000'000'000);
    CHECK(PacingClock::deadline_ns(anchor_wall, 0, 1'000'000'000, 0.0) == 0);

    CHECK(PacingClock::deadline_ns(1000, 100, 1100, 2.0) == 1500);
    CHECK(PacingClock::deadline_ns(123, 100, 100, 1.0) == 123);
}

TEST_CASE("PacingClock keeps speed and reanchors", "[control]")
{
    PacingClock clock(1.0, 100);
    REQUIRE(clock.speed() == 1.0);
    REQUIRE(clock.anchor_sim_ns() == 100);
    REQUIRE(clock.anchor_wall_ns() > 0);

    clock.apply_speed(2.0, 500);
    REQUIRE(clock.speed() == 2.0);
    REQUIRE(clock.anchor_sim_ns() == 500);

    clock.anchor_at(700);
    REQUIRE(clock.anchor_sim_ns() == 700);
    REQUIRE(clock.anchor_wall_ns() > 0);

    REQUIRE_THROWS_AS(PacingClock(-1.0, 0), std::invalid_argument);
}
