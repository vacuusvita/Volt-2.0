#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace bedrocktools::config {
using OptionValues = std::unordered_map<std::string, std::string>;
OptionValues parseOptions(const std::string& text);
std::string serializeOptions(const std::string& text, const OptionValues& values,
                             const std::unordered_set<std::string>& removed = {});
}
