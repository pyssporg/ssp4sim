#include <catch2/catch_test_macros.hpp>

#include "simulation/control/run_control.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace ssp4sim::control;

namespace
{
    bool wait_for_state(const RunControl &control, RunState expected,
                        std::chrono::milliseconds timeout = std::chrono::seconds(1))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (control.state() == expected)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return control.state() == expected;
    }
}

TEST_CASE("RunControl rejects invalid state transitions", "[control]")
{
    RunControl control(0.0, 0);

    REQUIRE_THROWS_AS(control.pause(), std::logic_error);
    REQUIRE_THROWS_AS(control.resume(), std::logic_error);
    REQUIRE_THROWS_AS(control.stop(), std::logic_error);
    REQUIRE_THROWS_AS(control.set_speed(1.0), std::logic_error);
    REQUIRE(control.state() == RunState::Created);

    control.mark_initialized();
    REQUIRE(control.state() == RunState::Initialized);
    REQUIRE_NOTHROW(control.set_speed(1.0));

    control.begin_run();
    REQUIRE(control.state() == RunState::Running);
    REQUIRE_THROWS_AS(control.begin_run(), std::logic_error);
    REQUIRE_THROWS_AS(control.resume(), std::logic_error);

    control.pause();
    REQUIRE(control.state() == RunState::Paused);
    REQUIRE_THROWS_AS(control.pause(), std::logic_error);

    control.resume();
    REQUIRE(control.state() == RunState::Running);
}

TEST_CASE("RunControl immediate pause resume and stop", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.pause();
    REQUIRE(control.state() == RunState::Paused);

    control.resume();
    REQUIRE(control.state() == RunState::Running);

    control.stop();
    REQUIRE(control.state() == RunState::Stopping);

    REQUIRE_FALSE(control.before_slice(0));

    RunControl paused(0.0, 0);
    paused.begin_run();
    paused.pause();
    paused.request_stop();
    REQUIRE(paused.state() == RunState::Stopping);
    REQUIRE_FALSE(paused.before_slice(0));
}

TEST_CASE("RunControl last speed wins", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.set_speed(2.0);
    control.set_speed(4.0);

    REQUIRE(control.speed() == 4.0);
    REQUIRE(control.before_slice(0));
    REQUIRE(control.speed() == 4.0);
}

TEST_CASE("RunControl validates speed values synchronously", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    REQUIRE_THROWS_AS(control.set_speed(-1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(control.set_speed(std::numeric_limits<double>::quiet_NaN()),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(control.set_speed(std::numeric_limits<double>::infinity()),
                      std::invalid_argument);
}

TEST_CASE("RunControl scheduled pause fires at its boundary", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.pause(1000);

    REQUIRE(control.before_slice(999));
    REQUIRE(control.state() == RunState::Running);

    std::packaged_task<bool()> task([&]() { return control.before_slice(1000); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    REQUIRE(wait_for_state(control, RunState::Paused));
    control.resume();

    REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    run_thread.join();
    REQUIRE(future.get());
    REQUIRE(control.state() == RunState::Running);
}

TEST_CASE("RunControl scheduled stop fires at its boundary", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.stop(1000);

    REQUIRE(control.before_slice(999));

    REQUIRE_FALSE(control.before_slice(1000));
    REQUIRE(control.state() == RunState::Stopping);
}

TEST_CASE("RunControl scheduled speed fires at its boundary", "[control]")
{
    RunControl control(1.0, 0);
    control.begin_run();

    control.set_speed(3.0, 1000);

    REQUIRE(control.before_slice(999));
    REQUIRE(control.speed() == 1.0);

    REQUIRE(control.before_slice(1000));
    REQUIRE(control.speed() == 3.0);
}

TEST_CASE("RunControl immediate command is not blocked by a scheduled one", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.pause(1000);
    control.set_speed(2.0);

    REQUIRE(control.before_slice(999));
    REQUIRE(control.speed() == 2.0);
    REQUIRE(control.state() == RunState::Running);
}

TEST_CASE("RunControl scheduled command in the past applies at the next boundary", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();

    control.pause(500);

    std::packaged_task<bool()> task([&]() { return control.before_slice(1000); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    REQUIRE(wait_for_state(control, RunState::Paused));
    control.resume();

    REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    run_thread.join();
    REQUIRE(future.get());
}

TEST_CASE("RunControl deadline math", "[control]")
{
    const int64_t anchor_wall = 1'000'000'000;

    CHECK(RunControl::compute_deadline_ns(anchor_wall, 0, 1'000'000'000, 1.0) ==
          2'000'000'000);
    CHECK(RunControl::compute_deadline_ns(anchor_wall, 0, 1'000'000'000, 2.0) ==
          1'500'000'000);
    CHECK(RunControl::compute_deadline_ns(anchor_wall, 0, 1'000'000'000, 0.5) ==
          3'000'000'000);
    CHECK(RunControl::compute_deadline_ns(anchor_wall, 0, 1'000'000'000, 0.0) ==
          0);

    CHECK(RunControl::compute_deadline_ns(1000, 100, 1100, 2.0) == 1500);
    CHECK(RunControl::compute_deadline_ns(123, 100, 100, 1.0) == 123);
}

TEST_CASE("RunControl resume wakes a paused run", "[control]")
{
    RunControl control(0.0, 0);
    control.begin_run();
    control.pause();
    REQUIRE(control.state() == RunState::Paused);

    std::packaged_task<bool()> task([&]() { return control.before_slice(0); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    control.resume();

    if (future.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
    {
        control.request_stop();
        run_thread.join();
        FAIL("resume did not wake the paused run");
    }

    run_thread.join();
    REQUIRE(future.get());
    REQUIRE(control.state() == RunState::Running);
}

TEST_CASE("RunControl stop interrupts paced wait", "[control]")
{
    RunControl control(1.0, 0);
    control.begin_run();

    std::packaged_task<bool()> task([&]() { return control.before_slice(5'000'000'000ULL); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    const auto stop_start = std::chrono::steady_clock::now();
    control.request_stop();

    REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    run_thread.join();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;

    REQUIRE_FALSE(future.get());
    REQUIRE(control.state() == RunState::Stopping);
    REQUIRE(stop_elapsed < std::chrono::seconds(1));
}

TEST_CASE("RunControl speed change interrupts paced wait", "[control]")
{
    RunControl control(1.0, 0);
    control.begin_run();

    std::packaged_task<bool()> task([&]() { return control.before_slice(5'000'000'000ULL); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    const auto change_start = std::chrono::steady_clock::now();
    control.set_speed(0.0);

    REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    run_thread.join();
    const auto change_elapsed = std::chrono::steady_clock::now() - change_start;

    REQUIRE(future.get());
    REQUIRE(change_elapsed < std::chrono::seconds(1));
}

TEST_CASE("RunControl pre-run stop cancels the run", "[control]")
{
    RunControl created(0.0, 0);
    created.request_stop();
    REQUIRE(created.state() == RunState::Stopped);
    REQUIRE_FALSE(created.begin_run());

    RunControl initialized(0.0, 0);
    initialized.mark_initialized();
    initialized.request_stop();
    REQUIRE(initialized.state() == RunState::Stopped);
    REQUIRE_FALSE(initialized.begin_run());
}

TEST_CASE("RunControl terminal transitions are monotonic", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.finish(RunResult::Completed);
    REQUIRE(control.state() == RunState::Completed);

    control.request_stop();
    REQUIRE(control.state() == RunState::Completed);

    control.finish(RunResult::Failed);
    REQUIRE(control.state() == RunState::Completed);

    REQUIRE_NOTHROW(control.stop());
    REQUIRE(control.state() == RunState::Completed);

    REQUIRE_THROWS_AS(control.begin_run(), std::logic_error);
}

TEST_CASE("RunControl stopping is preserved and stop is idempotent", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.stop();
    REQUIRE(control.state() == RunState::Stopping);

    REQUIRE_NOTHROW(control.stop());
    REQUIRE(control.state() == RunState::Stopping);

    REQUIRE_NOTHROW(control.request_stop());
    REQUIRE(control.state() == RunState::Stopping);

    control.finish(RunResult::Stopped);
    REQUIRE(control.state() == RunState::Stopped);

    REQUIRE_NOTHROW(control.stop());
    REQUIRE(control.state() == RunState::Stopped);
}

TEST_CASE("RunControl pre-run stop throws", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE_THROWS_AS(control.stop(), std::logic_error);

    control.mark_initialized();
    REQUIRE_THROWS_AS(control.stop(), std::logic_error);
}

TEST_CASE("RunControl publish_boundary publishes time and applies final stop", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.stop(1000);

    control.publish_boundary(1000);
    REQUIRE(control.state() == RunState::Stopping);
    REQUIRE(control.sim_time() == 1000);

    // Monotonic: a smaller boundary must not move time backwards.
    control.publish_boundary(500);
    REQUIRE(control.sim_time() == 1000);

    RunControl completed(0.0, 0);
    REQUIRE(completed.begin_run());
    completed.publish_boundary(2000);
    REQUIRE(completed.sim_time() == 2000);
    completed.finish(RunResult::Completed);
    REQUIRE(completed.state() == RunState::Completed);
    REQUIRE(completed.sim_time() == 2000);
}

TEST_CASE("RunControl publish_boundary advances time while stopping", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.stop();
    REQUIRE(control.state() == RunState::Stopping);

    control.publish_boundary(1500);
    REQUIRE(control.state() == RunState::Stopping);
    REQUIRE(control.sim_time() == 1500);

    // Terminal states still ignore the boundary.
    control.finish(RunResult::Stopped);
    control.publish_boundary(2500);
    REQUIRE(control.state() == RunState::Stopped);
    REQUIRE(control.sim_time() == 1500);
}

TEST_CASE("RunControl finish while stopping publishes Stopped", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.stop();
    REQUIRE(control.state() == RunState::Stopping);

    REQUIRE(control.finish(RunResult::Completed) == RunResult::Stopped);
    REQUIRE(control.state() == RunState::Stopped);

    REQUIRE(control.finish(RunResult::Failed) == RunResult::Stopped);
    REQUIRE(control.state() == RunState::Stopped);
}

TEST_CASE("RunControl equal-timestamp commands apply in insertion order", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());

    control.set_speed(2.0, 1000);
    control.stop(1000);

    REQUIRE_FALSE(control.before_slice(1000));
    REQUIRE(control.state() == RunState::Stopping);
    REQUIRE(control.speed() == 2.0);
}

TEST_CASE("RunControl paused due stop wakes and stops", "[control]")
{
    RunControl control(0.0, 0);
    REQUIRE(control.begin_run());
    control.pause();
    REQUIRE(control.state() == RunState::Paused);

    std::packaged_task<bool()> task([&]() { return control.before_slice(0); });
    auto future = task.get_future();
    std::thread run_thread(std::move(task));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    control.stop(0);
    REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    run_thread.join();
    REQUIRE_FALSE(future.get());
    REQUIRE(control.state() == RunState::Stopping);
}

TEST_CASE("RunControl RunInfo is coherent", "[control]")
{
    RunControl control(2.0, 100);

    RunInfo info = control.info();
    REQUIRE(info.state == RunState::Created);
    REQUIRE(info.sim_time_ns == 100);
    REQUIRE(info.speed == 2.0);

    control.mark_initialized();
    REQUIRE(control.begin_run());
    control.set_speed(4.0);
    control.publish_boundary(250);

    info = control.info();
    REQUIRE(info.state == RunState::Running);
    REQUIRE(info.sim_time_ns == 250);
    REQUIRE(info.speed == 4.0);
}
