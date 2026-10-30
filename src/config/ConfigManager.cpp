#include "ConfigManager.hpp"
#include "modules/ModuleRegistry.hpp"

#include <android/log.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace bedrocktools::config {
namespace {

constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/config_diag.log";

// The launcher stages mods into a private cache copy and loads the library from
// there, so ModContext::configDir() can point at the cache instead of the
// folder the mod menu edits. Rewrite ".../native_mods/<pkg>/..." onto the media
// tree and keep the configured path as a last resort.
std::vector<std::string> configCandidates(const std::string& configured) {
    std::vector<std::string> out;
    constexpr std::string_view marker = "native_mods/";
    const auto pos = configured.find(marker);
    if (pos != std::string::npos) {
        const std::string rest = configured.substr(pos + marker.size());
        const auto slash = rest.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < rest.size()) {
            const std::string pkg = rest.substr(0, slash);
            const std::string tail = rest.substr(slash + 1);
            const std::string media =
                "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/" + pkg + "/" + tail;
            out.push_back(media);
            if (media.rfind("/storage/emulated/0", 0) == 0) {
                out.push_back("/sdcard" + media.substr(std::string("/storage/emulated/0").size()));
            }
        }
    }
    out.push_back(configured);
    return out;
}

int applyModules(const nlohmann::json& root) {
    if (!root.contains("Modules") || !root["Modules"].is_object()) return 0;
    const auto& modulesObj = root["Modules"];
    int applied = 0;
    for (auto* mod : ModuleRegistry::get().modules()) {
        if (!modulesObj.contains(mod->name)) continue;
        try {
            mod->loadConfig(modulesObj[mod->name]);
            ++applied;
        } catch (...) {
        }
    }
    return applied;
}

}

void ConfigManager::log(const std::string& line) {
    __android_log_print(ANDROID_LOG_INFO, "VoltConfig", "%s", line.c_str());
    std::error_code ec;
    std::filesystem::path path(kDiagPath);
    if (!std::filesystem::exists(path.parent_path(), ec) || ec) return;
    if (FILE* file = std::fopen(kDiagPath, "a")) {
        std::fputs(line.c_str(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

std::string ConfigManager::getConfigPath() const {
    std::lock_guard lock(m_mutex);
    if (!m_configPath.empty()) return m_configPath;
    return "/sdcard/games/Volt/config.json";
}

void ConfigManager::setConfigPath(const std::string& path) {
    std::lock_guard lock(m_mutex);
    m_configPath = path;
}

nlohmann::json ConfigManager::defaultsJson() {
    nlohmann::json root;
    root["Modules"] = nlohmann::json::object();
    for (auto* mod : ModuleRegistry::get().modules()) {
        nlohmann::json entry;
        mod->saveConfig(entry);
        root["Modules"][mod->name] = std::move(entry);
    }
    return root;
}

bool ConfigManager::writeAtomically(const std::string& path, const std::string& payload) {
    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        ec.clear();
    }
    const std::filesystem::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

void ConfigManager::load() {
    std::lock_guard lock(m_mutex);
    const std::string configured = getConfigPath();
    const auto candidates = configCandidates(configured);
    log("load: configured=" + configured);

    std::string chosen;
    nlohmann::json chosenJson;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(candidate, ec) && !ec;
        log(std::string("  candidate ") + candidate + (exists ? " [exists]" : " [missing]"));
        if (!exists) continue;
        try {
            std::ifstream in(candidate);
            nlohmann::json j;
            in >> j;
            if (!j.contains("Modules") || !j["Modules"].is_object()) {
                log("    -> parsed, no Modules object, ignored");
                continue;
            }
            chosen = candidate;
            chosenJson = std::move(j);
            break;
        } catch (const std::exception& e) {
            log(std::string("    -> parse failed: ") + e.what() + " (file left untouched)");
            std::error_code copyEc;
            std::filesystem::copy_file(candidate, candidate + ".broken",
                                       std::filesystem::copy_options::overwrite_existing, copyEc);
        } catch (...) {
            log("    -> parse failed (file left untouched)");
        }
    }

    if (chosen.empty()) {
        // No usable file: build the defaults, apply them to the modules and
        // persist the same object. Applying is what the old build forgot.
        const nlohmann::json defaults = defaultsJson();
        if (writeAtomically(configured, defaults.dump(2))) {
            log("load: created " + configured + " with defaults");
        } else {
            log("load: could not create " + configured + ", defaults kept in memory");
        }
        setConfigPath(configured);
        const int applied = applyModules(defaults);
        log("load: applied " + std::to_string(applied) + " module entries (defaults)");
        return;
    }

    if (chosen != configured) {
        log("load: reading " + chosen);
        setConfigPath(chosen);
    }
    const int applied = applyModules(chosenJson);
    log("load: applied " + std::to_string(applied) + " of " +
        std::to_string(ModuleRegistry::get().modules().size()) + " module entries from " + chosen);
}

void ConfigManager::save() {
    std::lock_guard lock(m_mutex);
    const std::string path = getConfigPath();
    const std::string payload = defaultsJson().dump(2);
    if (!writeAtomically(path, payload)) log("save failed for " + path);
}

}
