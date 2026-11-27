#include "cpp_api/simulator_core.hpp"
#include "shared_config.hpp"
#include "runtime_control.hpp"
#include "simulation/serialize/serialize.hpp"
#include "cpp_api/simulator.hpp"

#include "ssp4cpp/utils/log.hpp"

#include "reference_ssp_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using ssp4sim::SharedConfig;
using ssp4sim::SimulatorCore;
using ssp4sim::StepStatus;

namespace
{
    /// Unique working directory, removed when the test ends.
    class TempDir
    {
    public:
        explicit TempDir(const std::string &tag)
        {
            static std::atomic<unsigned> counter{0};
            const auto stamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            path_ = fs::temp_directory_path() / "ssp4sim_snapshot_tests" /
                    (tag + "_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++));
            fs::remove_all(path_);
            fs::create_directories(path_);
        }

        ~TempDir()
        {
            std::error_code error;
            fs::remove_all(path_, error);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;

        const fs::path &path() const { return path_; }

    private:
        fs::path path_;
    };

    /// Builds the shared test config. Recording stays off unless `csv_file` is
    /// set; in that case CSV recording targets `csv_file` once per timestep.
    SharedConfig make_config(const fs::path &ssp_dir,
                             const fs::path &workdir,
                             double stop_time = 1.0,
                             const fs::path &csv_file = {})
    {
        const bool record_csv = !csv_file.empty();

        nlohmann::json csv_recording = {{"enable", record_csv}};
        if (record_csv)
        {
            csv_recording["file"] = csv_file.string();
            csv_recording["interval"] = 0.01; // seconds; equal to the timestep below
        }

        nlohmann::json config;
        config["simulation"] = {
            {"ssp", ssp_dir.string()},
            {"start_time", 0.0},
            {"stop_time", stop_time},
            {"timestep", 0.01},
            {"tolerance", 1e-6},
            {"working_dir", workdir.string()},
            {"executor", {{"method", "jacobi"}, {"forward_derivatives", false}}},
            {"log", {{"fmu", false},
                     {"level_terminal", "disable"},
                     {"level_file", "disable"},
                     {"level_json", "disable"},
                     {"level_cutelog", "disable"}}},
            {"recording", {{"csv", csv_recording},
                           {"sqlite", {{"enable", false}}},
                           {"wait_for", record_csv}}}};

        auto parsed = SharedConfig::from_json(config,
                                              ssp4cpp::utils::log::simple_logger());
        parsed.enable_recording = record_csv;
        parsed.csv.enable = record_csv;
        parsed.sqlite.enable = false;
        parsed.wait_for_recorder = record_csv;
        return parsed;
    }

    fs::path require_dahlquist()
    {
        const fs::path ssp_dir = ssp4sim::tests::reference_ssp::fixture_root("Dahlquist");
        REQUIRE(fs::exists(ssp_dir / "SystemStructure.ssd"));
        return ssp_dir;
    }

    std::string trim(std::string text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    /// The CSV sink writes `time, a, b` rows, so split on ',' and trim.
    std::vector<std::string> split_csv_row(const std::string &line)
    {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t comma = line.find(',', start);
            const std::size_t length =
                comma == std::string::npos ? std::string::npos : comma - start;
            fields.push_back(trim(line.substr(start, length)));
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
        return fields;
    }

    struct CsvTable
    {
        std::vector<std::string> header;
        std::vector<std::vector<std::string>> rows;
    };

    CsvTable read_csv(const fs::path &path)
    {
        std::ifstream input(path);
        REQUIRE(input.is_open());

        CsvTable table;
        std::string line;
        REQUIRE(std::getline(input, line));
        table.header = split_csv_row(line);

        while (std::getline(input, line))
        {
            if (trim(line).empty())
            {
                continue;
            }
            table.rows.push_back(split_csv_row(line));
        }
        return table;
    }

    struct SnapshotFile
    {
        nlohmann::json manifest;
        std::vector<std::byte> payload;
    };

    /// Minimal reader used only to compare two snapshot files. It mirrors the
    /// fixed v4 header; the library itself exposes no parser.
    SnapshotFile read_snapshot_file(const fs::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.is_open());

        std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        REQUIRE(bytes.size() >= 28);

        REQUIRE(std::string(bytes.data(), 8) == "SSP4SNAP");

        const auto read_u32 = [&](std::size_t offset)
        {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
            {
                value |= static_cast<std::uint32_t>(
                             static_cast<unsigned char>(bytes[offset + i]))
                         << (8 * i);
            }
            return value;
        };
        const auto read_u64 = [&](std::size_t offset)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
            {
                value |= static_cast<std::uint64_t>(
                             static_cast<unsigned char>(bytes[offset + i]))
                         << (8 * i);
            }
            return value;
        };

        REQUIRE(read_u32(8) == 4);
        const std::uint64_t manifest_size = read_u64(12);
        REQUIRE(28 + manifest_size <= bytes.size());

        SnapshotFile result;
        result.manifest = nlohmann::json::parse(
            std::string(bytes.begin() + 28,
                        bytes.begin() + 28 + manifest_size));

        const std::size_t payload_start = 28 + static_cast<std::size_t>(manifest_size);
        result.payload.resize(bytes.size() - payload_start);
        if (!result.payload.empty())
        {
            std::memcpy(result.payload.data(), bytes.data() + payload_start,
                        result.payload.size());
        }
        return result;
    }

    std::vector<char> read_file_bytes(const fs::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.is_open());
        return std::vector<char>((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
    }

    void write_file_bytes(const fs::path &path, const std::vector<char> &bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.is_open());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }

    std::size_t payload_offset(const std::vector<char> &bytes)
    {
        REQUIRE(bytes.size() >= 28);
        std::uint64_t manifest_size = 0;
        for (int i = 0; i < 8; ++i)
        {
            manifest_size |=
                static_cast<std::uint64_t>(
                    static_cast<unsigned char>(bytes[12 + i]))
                << (8 * i);
        }
        return 28 + static_cast<std::size_t>(manifest_size);
    }

    constexpr std::uint64_t fnv_offset_basis = 1469598103934665603ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;

    std::uint64_t fnv1a(const std::byte *data, std::size_t size,
                        std::uint64_t hash)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= std::to_integer<std::uint64_t>(data[i]);
            hash *= fnv_prime;
        }
        return hash;
    }

    std::string to_hex(std::uint64_t value)
    {
        std::ostringstream text;
        text << std::hex << std::setfill('0') << std::setw(16) << value;
        return text.str();
    }

    void append_bytes(std::vector<char> &out, const void *source,
                      std::size_t size)
    {
        if (size == 0)
        {
            return;
        }
        const auto *bytes = static_cast<const char *>(source);
        out.insert(out.end(), bytes, bytes + size);
    }

    void append_u32(std::vector<char> &out, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
        }
    }

    void append_u64(std::vector<char> &out, std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
        }
    }

    std::vector<char> build_snapshot_bytes(const nlohmann::json &manifest,
                                           const std::vector<std::byte> &payload)
    {
        const std::string text = manifest.dump();
        const std::uint64_t checksum =
            fnv1a(payload.data(), payload.size(),
                  fnv1a(reinterpret_cast<const std::byte *>(text.data()),
                        text.size(), fnv_offset_basis));

        std::vector<char> bytes;
        append_bytes(bytes, "SSP4SNAP", 8);
        append_u32(bytes, 4);
        append_u64(bytes, text.size());
        append_u64(bytes, checksum);
        append_bytes(bytes, text.data(), text.size());
        append_bytes(bytes, payload.data(), payload.size());
        return bytes;
    }

    void write_snapshot_file(const fs::path &path, const nlohmann::json &manifest,
                             const std::vector<std::byte> &payload)
    {
        write_file_bytes(path, build_snapshot_bytes(manifest, payload));
    }

    /// The reference FMU serializes its full component struct, including
    /// process-local callback/heap pointers. Those bytes legitimately differ
    /// between two FMU instances, so they are masked before comparing payloads;
    /// everything else (model storage and frontier bytes) must match exactly.
    std::vector<std::byte> comparable_payload(const SnapshotFile &file)
    {
        std::vector<std::byte> payload = file.payload;
        for (const auto &section : file.manifest.at("sections"))
        {
            if (section.at("kind").get<std::string>() != "model")
            {
                continue;
            }

            const std::size_t offset =
                section.at("offset").get<std::size_t>();
            const std::size_t size =
                section.at("size").get<std::size_t>();
            REQUIRE(offset + size <= payload.size());

            // Model layout: three identity strings, current time, blob length,
            // then the FMI state blob. Only that blob is process-local.
            ssp4sim::serialize::Reader reader(
                std::span<const std::byte>(payload.data() + offset, size));
            reader.read_string();
            reader.read_string();
            reader.read_string();
            reader.read_u64();
            const auto blob = reader.read_blob();

            REQUIRE(blob.data() >= payload.data());
            const std::size_t blob_offset = static_cast<std::size_t>(
                blob.data() - payload.data());
            std::fill(payload.begin() + blob_offset,
                      payload.begin() + blob_offset + blob.size(),
                      std::byte{0});
        }
        return payload;
    }
}

// ---------------------------------------------------------------------------
// A save->load->save cycle must reproduce the manifest and the payload byte for
// byte, except for the reference FMU's process-local pointer bytes (masked by
// comparable_payload).
// ---------------------------------------------------------------------------
TEST_CASE("snapshot round-trip is invariant", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("roundtrip");

    const fs::path first = dir.path() / "a.snap";
    const fs::path second = dir.path() / "b.snap";

    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();

        REQUIRE(stepper.save_snapshot(first));
        REQUIRE(fs::exists(first));
    }

    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_b"));
        stepper.init();
        stepper.load_snapshot(first);
        REQUIRE(stepper.save_snapshot(second));
    }

    SnapshotFile a = read_snapshot_file(first);
    SnapshotFile b = read_snapshot_file(second);

    REQUIRE(a.manifest == b.manifest);
    REQUIRE(comparable_payload(a) == comparable_payload(b));
}

// ---------------------------------------------------------------------------
// A mid-run snapshot restores the saved simulation time and resumes to the end.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot taken mid-run resumes and completes", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("midrun");

    const fs::path snapshot = dir.path() / "mid.snap";
    constexpr std::uint64_t saved_time_ns = 50'000'000; // 0.05 s

    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();
        stepper.start();
        REQUIRE(stepper.advance_to(saved_time_ns) == StepStatus::Advanced);
        REQUIRE(stepper.sim_time_ns() == saved_time_ns);
        stepper.save_snapshot(snapshot);
    }

    SimulatorCore restored(make_config(ssp_dir, dir.path() / "wd_b"));
    restored.init();
    restored.load_snapshot(snapshot);
    REQUIRE(restored.sim_time_ns() == saved_time_ns);

    restored.start();
    StepStatus status = StepStatus::Advanced;
    for (int i = 0; i < 1000 && status != StepStatus::Completed; ++i)
    {
        status = restored.step();
    }
    REQUIRE(status == StepStatus::Completed);
    REQUIRE(restored.sim_time_ns() == restored.end_time_ns());
}

// ---------------------------------------------------------------------------
// An SSP hash mismatch rejects the file before mutating the live stepper.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot SSP hash mismatch is rejected without mutation", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("mismatch");

    const fs::path snapshot = dir.path() / "dahlquist.snap";

    {
        SimulatorCore source(make_config(ssp_dir, dir.path() / "wd_a"));
        source.init();
        source.save_snapshot(snapshot);
    }

    SnapshotFile file = read_snapshot_file(snapshot);
    file.manifest["ssp_hash"] = "0000000000000000";
    const fs::path mismatched = dir.path() / "mismatched.snap";
    write_snapshot_file(mismatched, file.manifest, file.payload);

    SimulatorCore target(make_config(ssp_dir, dir.path() / "wd_b"));
    target.init();

    const std::uint64_t before = target.sim_time_ns();
    REQUIRE_THROWS_AS(target.load_snapshot(mismatched), std::runtime_error);
    REQUIRE(target.sim_time_ns() == before);
}

// ---------------------------------------------------------------------------
// Missing files and files with a bad magic are runtime errors.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot missing file and bad magic are rejected", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("invalid");

    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd"));
    stepper.init();

    REQUIRE_THROWS_AS(stepper.load_snapshot(dir.path() / "missing.snap"),
                      std::runtime_error);

    const fs::path bad_magic = dir.path() / "bad_magic.snap";
    {
        std::ofstream output(bad_magic, std::ios::binary | std::ios::trunc);
        REQUIRE(output.is_open());
        const std::vector<char> bytes(32, 'X');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE_THROWS_AS(stepper.load_snapshot(bad_magic), std::runtime_error);
}

// ---------------------------------------------------------------------------
// A run restored from a mid-run snapshot continues within tolerance of the
// uninterrupted run over the shared time range (IMP-054 acceptance clause).
// ---------------------------------------------------------------------------
TEST_CASE("restored run continues within tolerance of uninterrupted run",
          "[snapshot][continuity]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("continuity");

    constexpr std::uint64_t split_ns = 500'000'000; // 0.5 s, an exact timestep boundary
    // Relative-or-absolute agreement required between uninterrupted and restored rows.
    constexpr double tolerance = 1e-9;

    const fs::path uninterrupted_csv = dir.path() / "uninterrupted.csv";
    const fs::path prefix_csv = dir.path() / "prefix.csv";
    const fs::path restored_csv = dir.path() / "restored.csv";
    const fs::path snapshot = dir.path() / "mid.snap";

    const auto run_to_completion = [](SimulatorCore &stepper)
    {
        stepper.start();
        StepStatus status = StepStatus::Advanced;
        for (int i = 0; i < 1000 && status != StepStatus::Completed; ++i)
        {
            status = stepper.step();
        }
        REQUIRE(status == StepStatus::Completed);
        stepper.finish();
    };

    // Run A: uninterrupted reference.
    {
        SimulatorCore stepper(
            make_config(ssp_dir, dir.path() / "wd_a", 1.0, uninterrupted_csv));
        stepper.init();
        run_to_completion(stepper);
    }

    // Run B1: prefix run, snapshot at 0.5 s, then discarded.
    {
        SimulatorCore stepper(
            make_config(ssp_dir, dir.path() / "wd_b1", 1.0, prefix_csv));
        stepper.init();
        stepper.start();
        StepStatus status = StepStatus::Advanced;
        for (int i = 0; i < 1000 && status != StepStatus::Completed &&
                        stepper.sim_time_ns() < split_ns;
             ++i)
        {
            status = stepper.step();
        }
        REQUIRE(stepper.sim_time_ns() == split_ns);
        stepper.save_snapshot(snapshot);
        stepper.finish();
    }

    // Run B2: restored run resumed from the snapshot.
    {
        SimulatorCore stepper(
            make_config(ssp_dir, dir.path() / "wd_b2", 1.0, restored_csv));
        stepper.init();
        stepper.load_snapshot(snapshot);
        REQUIRE(stepper.sim_time_ns() == split_ns);
        run_to_completion(stepper);
    }

    const CsvTable a = read_csv(uninterrupted_csv);
    const CsvTable b = read_csv(restored_csv);

    REQUIRE(a.header == b.header);

    const double split_seconds = static_cast<double>(split_ns) / 1e9;
    std::vector<const std::vector<std::string> *> a_tail;
    for (const auto &row : a.rows)
    {
        REQUIRE(row.size() == a.header.size());
        if (std::stod(row.front()) > split_seconds)
        {
            a_tail.push_back(&row);
        }
    }

    INFO("uninterrupted tail rows=" << a_tail.size()
                                    << " restored rows=" << b.rows.size());
    REQUIRE(a_tail.size() == b.rows.size());

    for (std::size_t r = 0; r < a_tail.size(); ++r)
    {
        const auto &left = *a_tail[r];
        const auto &right = b.rows[r];
        REQUIRE(right.size() == b.header.size());
        REQUIRE(left.size() == right.size());

        for (std::size_t c = 0; c < left.size(); ++c)
        {
            const double lhs = std::stod(left[c]);
            const double rhs = std::stod(right[c]);
            const double delta = std::abs(lhs - rhs);
            const double scale = std::max({1.0, std::abs(lhs), std::abs(rhs)});
            INFO("row " << r << " column '" << a.header[c]
                        << "': uninterrupted=" << lhs << " restored=" << rhs
                        << " delta=" << delta);
            REQUIRE(delta <= tolerance * scale);
        }
    }
}

// ---------------------------------------------------------------------------
// A single corrupted payload byte is caught by the payload checksum before any
// state is applied.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot payload corruption is rejected", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("corrupt");

    const fs::path snapshot = dir.path() / "corrupt.snap";
    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();
        stepper.save_snapshot(snapshot);
    }

    std::vector<char> bytes = read_file_bytes(snapshot);
    const std::size_t payload_start = payload_offset(bytes);
    REQUIRE(payload_start < bytes.size());
    bytes.back() ^= 0x01; // flip one payload byte
    write_file_bytes(snapshot, bytes);

    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_b"));
    stepper.init();

    try
    {
        stepper.load_snapshot(snapshot);
        FAIL("expected SnapshotError for a corrupted payload");
    }
    catch (const ssp4sim::SnapshotError &error)
    {
        REQUIRE(error.code == "invalid_snapshot");
    }
}

// ---------------------------------------------------------------------------
// Only the current format is readable; an old format version is rejected.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot v1 format is rejected", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("v1-rejected");

    std::vector<char> bytes;
    append_bytes(bytes, "SSP4SNAP", 8);
    append_u32(bytes, 1);
    append_u64(bytes, 0);
    append_u64(bytes, 0);
    const fs::path path = dir.path() / "v1.snap";
    write_file_bytes(path, bytes);

    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd"));
    stepper.init();
    try
    {
        stepper.load_snapshot(path);
        FAIL("expected SnapshotError for a v1 snapshot");
    }
    catch (const ssp4sim::SnapshotError &error)
    {
        REQUIRE(error.code == "invalid_snapshot");
    }
}

// ---------------------------------------------------------------------------
// A snapshot rejected during apply throws apply_failed. The failed core is
// left unspecified; the caller discards it and builds a fresh one.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot apply failure throws apply_failed", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("apply-failure");

    const fs::path snapshot = dir.path() / "state.snap";
    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();
        stepper.save_snapshot(snapshot);
    }

    SnapshotFile file = read_snapshot_file(snapshot);
    REQUIRE_FALSE(file.manifest.at("sections").empty());
    REQUIRE(file.manifest["sections"][0]["kind"].get<std::string>() == "model");
    file.manifest["sections"][0]["size"] = 1;

    const fs::path wrong_state = dir.path() / "wrong_state.snap";
    write_snapshot_file(wrong_state, file.manifest, file.payload);

    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_b"));
    stepper.init();
    try
    {
        stepper.load_snapshot(wrong_state);
        FAIL("expected SnapshotError for a truncated model section");
    }
    catch (const ssp4sim::SnapshotError &error)
    {
        REQUIRE(error.code == "apply_failed");
    }
}

// ---------------------------------------------------------------------------
// A single corrupted manifest byte is caught by the content checksum before
// JSON parsing, so it can never be interpreted as a different manifest.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot manifest corruption is rejected", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("corrupt-manifest");

    const fs::path snapshot = dir.path() / "corrupt.snap";
    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();
        stepper.save_snapshot(snapshot);
    }

    std::vector<char> bytes = read_file_bytes(snapshot);
    const std::size_t payload_start = payload_offset(bytes);
    REQUIRE(28 < payload_start);
    bytes[28] ^= 0x01; // flip one manifest byte
    write_file_bytes(snapshot, bytes);

    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_b"));
    stepper.init();

    try
    {
        stepper.load_snapshot(snapshot);
        FAIL("expected SnapshotError for a corrupted manifest");
    }
    catch (const ssp4sim::SnapshotError &error)
    {
        REQUIRE(error.code == "invalid_snapshot");
        REQUIRE(std::string(error.what()).find("checksum") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Identity is fingerprint-based, so the same SSP reached through a symlink
// restores successfully.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot identity is independent of the SSP path", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("symlink");

    const fs::path link = dir.path() / "DahlquistLink";
    fs::create_directory_symlink(ssp_dir, link);
    REQUIRE(fs::is_directory(link));

    const fs::path snapshot = dir.path() / "identity.snap";
    {
        SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd_a"));
        stepper.init();
        stepper.save_snapshot(snapshot);
    }

    SimulatorCore restored(make_config(link, dir.path() / "wd_b"));
    restored.init();
    REQUIRE_NOTHROW(restored.load_snapshot(snapshot));
}

// ---------------------------------------------------------------------------
// A successful save atomically replaces the target and leaves no temp files.
// ---------------------------------------------------------------------------
TEST_CASE("snapshot save leaves no temporary file", "[snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("atomic");

    const fs::path snapshot = dir.path() / "atomic.snap";
    SimulatorCore stepper(make_config(ssp_dir, dir.path() / "wd"));
    stepper.init();
    stepper.save_snapshot(snapshot);

    REQUIRE(fs::exists(snapshot));
    for (const auto &entry : fs::directory_iterator(dir.path()))
    {
        const std::string name = entry.path().filename().string();
        INFO("leftover entry: " << name);
        REQUIRE(name.find(".part-") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// A blocking snapshot on a live Simulator captures the current mid-run state.
// ---------------------------------------------------------------------------
TEST_CASE("Simulator snapshot captures a running run", "[runner][snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("simulator-midrun");

    auto config = make_config(ssp_dir, dir.path() / "wd", 10.0);
    config.speed = 0.1;

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    runner.start();
    REQUIRE(runner.info().state == ssp4sim::RunState::Running);

    const fs::path snapshot = dir.path() / "midrun.snap";
    REQUIRE(runner.snapshot(snapshot));

    REQUIRE(fs::exists(snapshot));
    const SnapshotFile file = read_snapshot_file(snapshot);
    REQUIRE(file.manifest.at("sim_time_ns").get<std::uint64_t>() <=
            runner.info().sim_time_ns);

    runner.stop();
    runner.wait();
}

// ---------------------------------------------------------------------------
// Two threads may snapshot the same running Simulator concurrently; each call
// posts its own run work and both settle successfully.
// ---------------------------------------------------------------------------
TEST_CASE("two concurrent Simulator snapshots both succeed",
          "[runner][snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("concurrent");

    auto config = make_config(ssp_dir, dir.path() / "wd", 10.0);
    config.speed = 0.1;

    ssp4sim::Simulator runner(std::move(config));
    runner.init();
    runner.start();
    REQUIRE(runner.info().state == ssp4sim::RunState::Running);

    const fs::path a_path = dir.path() / "a.snap";
    const fs::path b_path = dir.path() / "b.snap";

    std::atomic<int> succeeded{0};
    std::string error_a;
    std::string error_b;

    std::thread first(
        [&]()
        {
            try
            {
                if (runner.snapshot(a_path))
                {
                    ++succeeded;
                }
            }
            catch (const std::exception &error)
            {
                error_a = error.what();
            }
        });
    std::thread second(
        [&]()
        {
            try
            {
                if (runner.snapshot(b_path))
                {
                    ++succeeded;
                }
            }
            catch (const std::exception &error)
            {
                error_b = error.what();
            }
        });

    first.join();
    second.join();

    REQUIRE(error_a.empty());
    REQUIRE(error_b.empty());
    REQUIRE(succeeded.load() == 2);
    REQUIRE(fs::exists(a_path));
    REQUIRE(fs::exists(b_path));

    runner.stop();
    runner.wait();
}

// ---------------------------------------------------------------------------
// Snapshot is only legal while the run is live; pre-run and terminal states
// throw before any run work is posted.
// ---------------------------------------------------------------------------
TEST_CASE("Simulator snapshot outside Running or Paused throws",
          "[runner][snapshot]")
{
    const fs::path ssp_dir = require_dahlquist();
    TempDir dir("not-live");

    auto config = make_config(ssp_dir, dir.path() / "wd", 1.0);
    ssp4sim::Simulator runner(std::move(config));

    REQUIRE_THROWS_AS(runner.snapshot(dir.path() / "created.snap"),
                      std::logic_error);

    runner.init();
    REQUIRE_THROWS_AS(runner.snapshot(dir.path() / "initialized.snap"),
                      std::logic_error);

    REQUIRE_FALSE(fs::exists(dir.path() / "created.snap"));
    REQUIRE_FALSE(fs::exists(dir.path() / "initialized.snap"));
}
