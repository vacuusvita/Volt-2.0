#include "OptionsFile.hpp"
#include <sstream>

namespace bedrocktools::config {
namespace {
std::string optTrim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}

OptionValues parseOptions(const std::string& text) {
    OptionValues keys;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        keys[optTrim(line.substr(0, colon))] = optTrim(line.substr(colon + 1));
    }
    return keys;
}

std::string serializeOptions(const std::string& originalText,
                             const OptionValues& keys,
                             const std::unordered_set<std::string>& removed) {
    std::ostringstream out;
    std::unordered_set<std::string> written;
    std::istringstream stream(originalText);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            out << line << '\n';
            continue;
        }
        const auto key = optTrim(line.substr(0, colon));
        if (removed.contains(key)) continue;
        const auto it = keys.find(key);
        if (it != keys.end()) {
            out << key << ':' << it->second << '\n';
            written.insert(key);
        } else {
            out << line << '\n';
        }
    }
    for (const auto& [key, value] : keys) {
        if (written.contains(key)) continue;
        out << key << ':' << value << '\n';
    }
    return out.str();
}

}
