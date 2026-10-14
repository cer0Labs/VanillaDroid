#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vd {

// client.cfg (config/client.example.cfg): "key = value" lines, '#' starts a comment. Read once at startup.
class Config {
public:
    // A missing file is an empty configuration.
    static Config load(const std::filesystem::path& path);

    std::string get(std::string_view key, std::string_view fallback = {}) const;
    bool has(std::string_view key) const { return m_values.count(std::string(key)) != 0; }

private:
    std::unordered_map<std::string, std::string> m_values;
};

} // namespace vd
