#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ssp4sim
{
    struct SharedConfig;

    namespace pre
    {
        struct SimulationGraph;
    }

    namespace snapshot
    {
        /// Everything the snapshot code needs from a live core. Borrowed for
        /// the duration of one save/load call; never stored.
        struct Context
        {
            SharedConfig *conf = nullptr;
            pre::SimulationGraph *graph = nullptr;
            uint64_t *sim_time_ns = nullptr;
            /// Cached SSP content hash, or null to compute it on demand.
            const std::string *fingerprint = nullptr;
        };

        /// Internal return value of one save (the public API reports success).
        struct SaveSummary
        {
            uint64_t sim_time_ns = 0;
            std::size_t model_count = 0;
            std::size_t file_bytes = 0;
        };

        /// SSP content hash of a file or directory: FNV-1a 64 over its contents
        /// (or its sorted relative paths and contents), as 16 hex characters.
        std::string fingerprint(const std::filesystem::path &ssp);

        /// Write a self-describing snapshot file. Throws SnapshotError if
        /// any model cannot get/set and serialize FMU state, if no resolver is
        /// installed, or on I/O failure.
        SaveSummary save(const Context &context,
                         const std::filesystem::path &path);

        /// Validate \p path fully against the live context, then apply it.
        /// Nothing is mutated before validation succeeds. The caller has
        /// already checked the core lifecycle.
        void load(const Context &context,
                  const std::filesystem::path &path);
    }
}
