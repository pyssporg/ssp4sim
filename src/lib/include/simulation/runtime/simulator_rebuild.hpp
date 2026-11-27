#pragma once

#include "cpp_api/simulator.hpp"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace ssp4sim::detail
{
    struct RebuildResult
    {
        std::unique_ptr<Simulator> replacement;  // null on failure
        std::exception_ptr exception;             // preserves the original type
        std::string message;                      // human message (empty on success)
        RunInfo old_info{};
        std::string old_run_error;
        bool had_run = false;
    };

    /// Quiesce \p old (stop/join when it had a run), then build a fresh
    /// simulator from \p config. A null \p snapshot builds an initialized
    /// simulator (reset); a non-null path builds a restored one (restore,
    /// carrying the old run's pace and rotating the CSV segment). Never throws;
    /// failures land in the result.
    struct RebuildAccess
    {
        static RebuildResult rebuild(Simulator *old, SharedConfig config,
                                     const std::filesystem::path *snapshot);
    };
}
