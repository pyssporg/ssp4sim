#include "utils/config.hpp"

#include "utils/time/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssp4sim::utils
{

    namespace
    {
        // Resolve "a.b.c" into a json node pointer (or nullptr if missing).
        const nlohmann::json *resolvePathNode(const nlohmann::json &data, const std::string &dottedKey)
        {
            const nlohmann::json *cur = &data;
            if (dottedKey.empty())
            {
                return cur;
            }

            size_t pos = 0;
            while (cur && pos != std::string::npos)
            {
                size_t dot = dottedKey.find('.', pos);
                std::string key = (dot == std::string::npos)
                                      ? dottedKey.substr(pos)
                                      : dottedKey.substr(pos, dot - pos);

                if (!key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char ch)
                                                { return std::isdigit(ch) != 0; }))
                {
                    size_t idx = static_cast<size_t>(std::stoul(key));
                    if (!cur->is_array() || idx >= cur->size())
                    {
                        return nullptr;
                    }
                    cur = &(*cur)[idx];
                }
                else
                {
                    if (!cur->is_object() || !cur->contains(key))
                    {
                        return nullptr;
                    }
                    cur = &(*cur)[key];
                }

                if (dot == std::string::npos)
                {
                    break;
                }
                pos = dot + 1;
            }
            return cur;
        }

        template <typename T>
        T read_json_value(const nlohmann::json &node)
        {
            return node.get<T>();
        }

        template <>
        std::string read_json_value<std::string>(const nlohmann::json &node)
        {
            return Config::substitute_tags(node.get<std::string>());
        }

        template <typename T>
        T get_value(const nlohmann::json &data, const std::string &dottedKey)
        {
            const nlohmann::json *node = resolvePathNode(data, dottedKey);
            if (!node || node->is_null())
            {
                throw missing_key_error("Config: missing key: " + dottedKey);
            }

            try
            {
                return read_json_value<T>(*node);
            }
            catch (const std::exception &e)
            {
                throw type_error("Config: type error for key '" + dottedKey +
                                 "': " + std::string(e.what()));
            }
        }
    }

    Config::Config(nlohmann::json data) : data_(std::move(data))
    {
    }

    Config Config::fromFile(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("Config: cannot open file: " + path.string());
        }

        std::ostringstream buf;
        buf << in.rdbuf();
        return fromString(buf.str());
    }

    Config Config::fromString(std::string_view config)
    {
        nlohmann::json parsed = nlohmann::json::parse(config.begin(), config.end(),
                                                      /* callback */ nullptr,
                                                      /* allow exceptions */ true,
                                                      /* ignore_comments */ true);

        return Config(std::move(parsed));
    }

    const nlohmann::json *Config::resolvePath(const std::string &dottedKey) const
    {
        return resolvePathNode(data_, dottedKey);
    }

    std::string Config::as_string() const
    {
        return data_.dump();
    }

    std::string Config::substitute_tags(std::string text)
    {
        return std::regex_replace(text, std::regex("\\[TIME\\]"), time::time_now_str());
    }

    std::string Config::getString(const std::string &dottedKey) const
    {
        return get_value<std::string>(data_, dottedKey);
    }

    double Config::getDouble(const std::string &dottedKey) const
    {
        return get_value<double>(data_, dottedKey);
    }

    int Config::getInt(const std::string &dottedKey) const
    {
        return get_value<int>(data_, dottedKey);
    }

    bool Config::getBool(const std::string &dottedKey) const
    {
        return get_value<bool>(data_, dottedKey);
    }

    std::string Config::getOr(const std::string &dottedKey, std::string_view defaultValue) const
    {
        try
        {
            return getString(dottedKey);
        }
        catch (const missing_key_error &)
        {
            return substitute_tags(std::string(defaultValue));
        }
    }

    std::string Config::getOr(const std::string &dottedKey, const char *defaultValue) const
    {
        try
        {
            return getString(dottedKey);
        }
        catch (const missing_key_error &)
        {
            return substitute_tags(std::string(defaultValue));
        }
    }

    double Config::getOr(const std::string &dottedKey, double defaultValue) const
    {
        try
        {
            return getDouble(dottedKey);
        }
        catch (const missing_key_error &)
        {
            return defaultValue;
        }
    }

    int Config::getOr(const std::string &dottedKey, int defaultValue) const
    {
        try
        {
            return getInt(dottedKey);
        }
        catch (const missing_key_error &)
        {
            return defaultValue;
        }
    }

    bool Config::getOr(const std::string &dottedKey, bool defaultValue) const
    {
        try
        {
            return getBool(dottedKey);
        }
        catch (const missing_key_error &)
        {
            return defaultValue;
        }
    }

}
