#include "core/Config.h"

#include <fstream>

namespace vd {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

Config Config::load(const std::filesystem::path& path) {
    Config config;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::string_view text(line);
        if (const auto hash = text.find('#'); hash != std::string_view::npos) {
            text = text.substr(0, hash);
        }
        const auto equals = text.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(text.substr(0, equals));
        if (!key.empty()) {
            config.m_values[std::string(key)] = std::string(trim(text.substr(equals + 1)));
        }
    }
    return config;
}

std::string Config::get(std::string_view key, std::string_view fallback) const {
    const auto it = m_values.find(std::string(key));
    return it == m_values.end() ? std::string(fallback) : it->second;
}

} // namespace vd
