#pragma once

#include "utils/time/time.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssp4sim::utils
{

    struct config_error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    struct missing_key_error : config_error
    {
        using config_error::config_error;
    };
    struct type_error : config_error
    {
        using config_error::config_error;
    };

    /// Value type for one parsed JSON config document. No process-global state:
    /// each instance owns its data, so several configs can coexist.
    class Config
    {
    public:
        Config() = default;
        explicit Config(nlohmann::json data);

        // Load a JSON config from a file path. Throws std::runtime_error on
        // I/O or parse errors.
        static Config fromFile(const std::filesystem::path &path);
        static Config fromString(std::string_view config);

        std::string as_string() const;

        std::string getString(const std::string &dottedKey) const;
        double getDouble(const std::string &dottedKey) const;
        int getInt(const std::string &dottedKey) const;
        bool getBool(const std::string &dottedKey) const;

        std::string getOr(const std::string &dottedKey, std::string_view defaultValue) const;
        std::string getOr(const std::string &dottedKey, const char *defaultValue) const;
        double getOr(const std::string &dottedKey, double defaultValue) const;
        int getOr(const std::string &dottedKey, int defaultValue) const;
        bool getOr(const std::string &dottedKey, bool defaultValue) const;

        const nlohmann::json *resolvePath(const std::string &dottedKey) const;

        static std::string substitute_tags(std::string text);

    private:
        nlohmann::json data_{};
    };

}
