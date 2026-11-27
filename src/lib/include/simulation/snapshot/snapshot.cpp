#include "simulation/snapshot/snapshot.hpp"

#include "cpp_api/simulator_core.hpp"

#include "shared_config.hpp"

#include "pre/pre_pipeline.hpp"
#include "pre/3_simulation_graph/elements/model_fmu.hpp"

#include "simulation/resolver/data_access_resolver.hpp"
#include "simulation/serialize/serialize.hpp"
#include "utils/primitives/uuid.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssp4sim::snapshot
{
    namespace
    {
        using nlohmann::json;

        constexpr std::string_view magic = "SSP4SNAP";
        constexpr std::uint32_t format_version = 4;
        constexpr std::size_t header_size = 28;
        constexpr std::uint64_t fnv_offset_basis = 1469598103934665603ULL;
        constexpr std::uint64_t fnv_prime = 1099511628211ULL;

        [[noreturn]] void fail(const std::string &message)
        {
            throw SnapshotError("invalid_snapshot", "snapshot: " + message);
        }

        [[noreturn]] void fail_io(const std::string &message)
        {
            throw SnapshotError("io_error", "snapshot: " + message);
        }

        [[noreturn]] void fail_unsupported(const std::string &message)
        {
            throw SnapshotError("unsupported", "snapshot: " + message);
        }

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

        // --- little-endian helpers ------------------------------------------

        std::uint32_t read_u32(const std::vector<std::byte> &data, std::size_t offset)
        {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
            {
                value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset + i])) << (8 * i);
            }
            return value;
        }

        std::uint64_t read_u64(const std::vector<std::byte> &data, std::size_t offset)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
            {
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[offset + i])) << (8 * i);
            }
            return value;
        }

        // --- file helpers ---------------------------------------------------

        std::vector<std::byte> read_file(const std::filesystem::path &path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                fail_io("cannot open '" + path.string() + "' for reading");
            }

            input.seekg(0, std::ios::end);
            const std::streamoff size = input.tellg();
            if (size < 0)
            {
                fail_io("cannot determine size of '" + path.string() + "'");
            }
            input.seekg(0, std::ios::beg);

            std::vector<std::byte> data(static_cast<std::size_t>(size));
            if (!data.empty())
            {
                input.read(reinterpret_cast<char *>(data.data()), size);
                if (!input)
                {
                    fail_io("short read from '" + path.string() + "'");
                }
            }
            return data;
        }

        // --- atomic writer --------------------------------------------------

        void write_u32(std::ostream &out, std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.put(static_cast<char>((value >> (8 * i)) & 0xffu));
            }
        }

        void write_u64(std::ostream &out, std::uint64_t value)
        {
            for (int i = 0; i < 8; ++i)
            {
                out.put(static_cast<char>((value >> (8 * i)) & 0xffu));
            }
        }

        /// Flush \p path to stable storage, reporting any failure as io_error.
        void fsync_path(const std::filesystem::path &path)
        {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0)
            {
                fail_io("cannot open '" + path.string() + "' for fsync: " +
                        std::strerror(errno));
            }
            if (::fsync(fd) != 0)
            {
                const int error = errno;
                ::close(fd);
                fail_io("fsync failed for '" + path.string() + "': " +
                        std::strerror(error));
            }
            if (::close(fd) != 0)
            {
                fail_io("cannot close '" + path.string() + "' after fsync: " +
                        std::strerror(errno));
            }
        }

        /// Directory durability is best-effort: file contents are already
        /// strictly flushed, but some filesystems reject directory fsync.
        void fsync_directory_best_effort(const std::filesystem::path &path)
        {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0)
            {
                return;
            }
            ::fsync(fd);
            ::close(fd);
        }

        /// Temporary sibling of \p target: same directory, unique name.
        std::filesystem::path temporary_path(const std::filesystem::path &target)
        {
            std::filesystem::path temp = target;
            temp += ".part-" + utils::make_uuid_v4();
            return temp;
        }

        /// Write header, manifest and payload sequentially to a sibling temp
        /// file, then rename it over \p path. The target is never truncated or
        /// removed before the new file is complete. Returns the total file size.
        std::size_t write_snapshot_file(const std::filesystem::path &path,
                                        const std::string &manifest_text,
                                        const std::vector<std::byte> &payload)
        {
            const std::filesystem::path temp = temporary_path(path);

            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                fail_io("cannot open '" + temp.string() + "' for writing");
            }

            const auto discard_temp = [&temp]()
            {
                std::error_code error;
                std::filesystem::remove(temp, error);
            };

            const std::uint64_t checksum =
                fnv1a(payload.data(), payload.size(),
                      fnv1a(reinterpret_cast<const std::byte *>(manifest_text.data()),
                            manifest_text.size(), fnv_offset_basis));

            try
            {
                output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
                write_u32(output, format_version);
                write_u64(output, static_cast<std::uint64_t>(manifest_text.size()));
                write_u64(output, checksum);
                output.write(manifest_text.data(),
                             static_cast<std::streamsize>(manifest_text.size()));
                if (!payload.empty())
                {
                    output.write(reinterpret_cast<const char *>(payload.data()),
                                 static_cast<std::streamsize>(payload.size()));
                }
                output.flush();
                if (!output)
                {
                    fail_io("failed writing '" + temp.string() + "'");
                }
                output.close();
                if (output.fail())
                {
                    fail_io("failed closing '" + temp.string() + "'");
                }
                fsync_path(temp);
            }
            catch (...)
            {
                discard_temp();
                throw;
            }

            std::error_code error;
            std::filesystem::rename(temp, path, error);
            if (error)
            {
                discard_temp();
                fail_io("cannot rename '" + temp.string() + "' to '" + path.string() +
                        "': " + error.message());
            }

            const std::filesystem::path directory =
                path.parent_path().empty() ? std::filesystem::path(".")
                                           : path.parent_path();
            fsync_directory_best_effort(directory);

            return header_size + manifest_text.size() + payload.size();
        }

        std::span<const std::byte> payload_range(const std::vector<std::byte> &file,
                                                 std::size_t payload_start,
                                                 std::size_t offset,
                                                 std::size_t size,
                                                 const std::string &what)
        {
            const std::size_t payload_size = file.size() - payload_start;
            if (offset > payload_size || size > payload_size - offset)
            {
                fail(what + " lies outside the payload");
            }
            return std::span<const std::byte>(file.data() + payload_start + offset, size);
        }

        // --- fingerprint ----------------------------------------------------

        struct FileHasher
        {
            std::uint64_t hash = fnv_offset_basis;

            void add_file(const std::filesystem::path &path)
            {
                std::ifstream input(path, std::ios::binary);
                if (!input.is_open())
                {
                    fail_io("cannot read '" + path.string() + "' while fingerprinting");
                }

                std::vector<char> buffer(64 * 1024);
                while (input)
                {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const std::streamsize count = input.gcount();
                    if (count > 0)
                    {
                        hash = fnv1a(reinterpret_cast<const std::byte *>(buffer.data()),
                                     static_cast<std::size_t>(count), hash);
                    }
                }
            }
        };

        std::string to_hex(std::uint64_t value)
        {
            std::ostringstream text;
            text << std::hex << std::setfill('0') << std::setw(16) << value;
            return text.str();
        }

        std::string compute_fingerprint(const std::filesystem::path &ssp)
        {
            FileHasher hasher;

            if (std::filesystem::is_regular_file(ssp))
            {
                hasher.add_file(ssp);
            }
            else if (std::filesystem::is_directory(ssp))
            {
                std::vector<std::filesystem::path> files;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(ssp))
                {
                    if (entry.is_regular_file())
                    {
                        files.push_back(std::filesystem::relative(entry.path(), ssp));
                    }
                }

                std::sort(files.begin(), files.end(),
                          [](const auto &lhs, const auto &rhs)
                          {
                              return lhs.generic_string() < rhs.generic_string();
                          });

                const std::byte zero{0};
                for (const auto &relative : files)
                {
                    const std::string text = relative.generic_string();
                    hasher.hash = fnv1a(reinterpret_cast<const std::byte *>(text.data()),
                                        text.size(), hasher.hash);
                    hasher.hash = fnv1a(&zero, 1, hasher.hash);
                    hasher.add_file(ssp / relative);
                }
            }
            else
            {
                fail_io("SSP path '" + ssp.string() + "' is neither a file nor a directory");
            }

            return to_hex(hasher.hash);
        }

        // --- misc -----------------------------------------------------------

        std::map<std::string, graph::FmuModel *> live_models(const pre::SimulationGraph &graph)
        {
            std::map<std::string, graph::FmuModel *> models;
            for (const auto &[name, node] : graph.models)
            {
                auto *model = dynamic_cast<graph::FmuModel *>(node.get());
                if (model != nullptr)
                {
                    models.emplace(name, model);
                }
            }
            return models;
        }

        [[nodiscard]] bool same_bytes(std::span<const std::byte> data,
                                      std::string_view expected)
        {
            if (data.size() != expected.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                if (std::to_integer<char>(data[i]) != expected[i])
                {
                    return false;
                }
            }
            return true;
        }

        void require_capability(graph::FmuModel &model, const std::string &name)
        {
            if (!model.fmu || !model.fmu->fmi_instance || !model.fmu->model)
            {
                fail_unsupported("model '" + name + "' has no FMU instance");
            }
            if (!model.fmu->fmi_instance->can_get_and_set_state() ||
                !model.fmu->fmi_instance->can_serialize_state())
            {
                fail_unsupported("model '" + name + "' cannot get/set and serialize FMU state");
            }
        }

        scheduling::DataAccessResolver &require_resolver(
            const std::map<std::string, graph::FmuModel *> &models)
        {
            if (models.empty())
            {
                fail("no FMU models in the simulation graph");
            }
            auto *resolver = models.begin()->second->access_resolver.get();
            if (resolver == nullptr)
            {
                fail("no access resolver installed on the first FMU model");
            }
            return *resolver;
        }

        // --- save -----------------------------------------------------------

        SaveSummary save_impl(const Context &context, const std::filesystem::path &path)
        {
            if (context.conf == nullptr || context.graph == nullptr || context.sim_time_ns == nullptr)
            {
                fail("save called without a complete context");
            }

            const auto models = live_models(*context.graph);
            scheduling::DataAccessResolver &resolver = require_resolver(models);

            // All-or-nothing: reject any model without the required
            // capabilities before serializing anything.
            for (const auto &[name, model] : models)
            {
                require_capability(*model, name);
            }

            std::vector<std::byte> payload;
            json manifest;

            manifest["ssp_hash"] = context.fingerprint != nullptr
                                       ? *context.fingerprint
                                       : compute_fingerprint(context.conf->ssp_path);
            manifest["sim_time_ns"] = *context.sim_time_ns;
            manifest["sections"] = json::array();

            const auto append_section = [&](const std::string &kind,
                                            const std::string *name,
                                            const std::vector<std::byte> &bytes)
            {
                const std::size_t offset = payload.size();
                payload.insert(payload.end(), bytes.begin(), bytes.end());

                json section = {{"kind", kind},
                                {"offset", offset},
                                {"size", bytes.size()}};
                if (name != nullptr)
                {
                    section["name"] = *name;
                }
                manifest["sections"].push_back(std::move(section));
            };

            try
            {
                for (const auto &[name, model] : models)
                {
                    serialize::Writer writer;
                    model->serialize(writer);
                    append_section("model", &name, writer.data());
                }

                serialize::Writer writer;
                resolver.serialize(writer);
                append_section("frontiers", nullptr, writer.data());
            }
            catch (const serialize::Unsupported &error)
            {
                fail_unsupported(error.what());
            }
            catch (const serialize::Error &error)
            {
                fail(error.what());
            }

            std::string manifest_text;
            try
            {
                manifest_text = manifest.dump();
            }
            catch (const std::exception &error)
            {
                throw SnapshotError("unsupported",
                                    std::string("snapshot: cannot encode string value: ") +
                                        error.what());
            }
            const std::size_t file_bytes =
                write_snapshot_file(path, manifest_text, payload);

            return {*context.sim_time_ns, models.size(), file_bytes};
        }

        // --- load -----------------------------------------------------------

        void load_impl(const Context &context, const std::filesystem::path &path)
        {
            if (context.conf == nullptr || context.graph == nullptr || context.sim_time_ns == nullptr)
            {
                fail("load called without a complete context");
            }

            const std::vector<std::byte> file = read_file(path);
            if (file.size() < header_size)
            {
                fail("file is shorter than the snapshot header");
            }
            if (!same_bytes(std::span<const std::byte>(file.data(), magic.size()), magic))
            {
                fail("bad magic in '" + path.string() + "'");
            }

            const std::uint32_t version = read_u32(file, 8);
            if (version != format_version)
            {
                fail("unsupported format version in '" + path.string() + "'");
            }

            const std::uint64_t manifest_size = read_u64(file, 12);
            if (manifest_size > file.size() - header_size)
            {
                fail("manifest size lies outside the file");
            }
            const std::size_t payload_start = header_size + static_cast<std::size_t>(manifest_size);
            const std::size_t payload_size = file.size() - payload_start;

            const std::uint64_t expected_checksum = read_u64(file, 20);
            const std::uint64_t live_checksum =
                fnv1a(file.data() + payload_start, payload_size,
                      fnv1a(file.data() + header_size,
                            static_cast<std::size_t>(manifest_size),
                            fnv_offset_basis));
            if (live_checksum != expected_checksum)
            {
                fail("content checksum mismatch");
            }

            const json manifest = json::parse(
                reinterpret_cast<const char *>(file.data() + header_size),
                reinterpret_cast<const char *>(file.data() + payload_start),
                nullptr, false);
            if (manifest.is_discarded())
            {
                fail("manifest is not valid JSON");
            }

            // SSP identity. The content hash identifies the model set
            // independently of where the SSP tree happens to live.
            const std::string live_fingerprint =
                context.fingerprint != nullptr
                    ? *context.fingerprint
                    : compute_fingerprint(context.conf->ssp_path);
            if (manifest.at("ssp_hash").get<std::string>() != live_fingerprint)
            {
                fail("SSP hash mismatch");
            }

            const std::uint64_t saved_sim_time = manifest.at("sim_time_ns").get<std::uint64_t>();
            if (saved_sim_time < context.conf->fmu.start_time ||
                saved_sim_time > context.conf->fmu.end_time)
            {
                fail("sim_time_ns lies outside the configured experiment span");
            }

            const auto models = live_models(*context.graph);
            scheduling::DataAccessResolver &resolver = require_resolver(models);

            struct ModelSection
            {
                graph::FmuModel *model = nullptr;
                std::span<const std::byte> bytes;
            };

            std::vector<ModelSection> model_sections;
            std::span<const std::byte> frontiers_bytes;
            bool has_frontiers = false;
            std::set<std::string> seen_models;

            const auto &sections = manifest.at("sections");
            if (!sections.is_array())
            {
                fail("sections is not an array");
            }
            for (const auto &entry : sections)
            {
                const std::string kind = entry.at("kind").get<std::string>();
                const std::size_t offset = entry.at("offset").get<std::size_t>();
                const std::size_t size = entry.at("size").get<std::size_t>();
                const auto bytes = payload_range(file, payload_start, offset, size,
                                                 "section '" + kind + "'");

                if (kind == "model")
                {
                    const std::string name = entry.at("name").get<std::string>();
                    if (!seen_models.insert(name).second)
                    {
                        fail("duplicate model section '" + name + "'");
                    }
                    const auto live_it = models.find(name);
                    if (live_it == models.end())
                    {
                        fail("model '" + name + "' is not a live model");
                    }
                    // Capability failures are reported before any state changes.
                    require_capability(*live_it->second, name);
                    model_sections.push_back({live_it->second, bytes});
                }
                else if (kind == "frontiers")
                {
                    if (has_frontiers)
                    {
                        fail("duplicate frontiers section");
                    }
                    has_frontiers = true;
                    frontiers_bytes = bytes;
                }
                else
                {
                    fail("unknown section kind '" + kind + "'");
                }
            }

            if (model_sections.size() != models.size())
            {
                fail("model set mismatch");
            }
            for (const auto &entry : models)
            {
                if (seen_models.find(entry.first) == seen_models.end())
                {
                    fail("model '" + entry.first + "' is missing from the manifest");
                }
            }
            if (!has_frontiers)
            {
                fail("missing frontiers section");
            }

            // --- apply (validation complete) --------------------------------

            try
            {
                for (const ModelSection &section : model_sections)
                {
                    serialize::Reader reader(section.bytes);
                    section.model->deserialize(reader);
                    reader.expect_end();
                }
                {
                    serialize::Reader reader(frontiers_bytes);
                    resolver.deserialize(reader);
                    reader.expect_end();
                }
                *context.sim_time_ns = saved_sim_time;
            }
            catch (const std::exception &error)
            {
                throw SnapshotError("apply_failed",
                                    std::string("snapshot: apply failed: ") +
                                        error.what());
            }
            catch (...)
            {
                throw SnapshotError("apply_failed",
                                    "snapshot: apply failed: unknown error");
            }
        }
    }

    std::string fingerprint(const std::filesystem::path &ssp)
    {
        return compute_fingerprint(ssp);
    }

    SaveSummary save(const Context &context, const std::filesystem::path &path)
    {
        try
        {
            return save_impl(context, path);
        }
        catch (const SnapshotError &)
        {
            throw;
        }
        catch (const std::exception &error)
        {
            throw SnapshotError("invalid_snapshot",
                                std::string("snapshot: ") + error.what());
        }
    }

    void load(const Context &context, const std::filesystem::path &path)
    {
        try
        {
            load_impl(context, path);
        }
        catch (const SnapshotError &)
        {
            throw;
        }
        catch (const std::exception &error)
        {
            throw SnapshotError("invalid_snapshot",
                                std::string("snapshot: ") + error.what());
        }
    }
}
