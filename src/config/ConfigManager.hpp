#pragma once

#include <nlohmann/json.hpp>
#include <mutex>
#include <string>

namespace bedrocktools::config {

// Reads/writes the mod config next to the installed mod.
//
// Rules that the previous implementation got wrong and this one enforces:
//   * a missing file is created with the current module defaults AND those
//     defaults are applied to the modules in the same pass;
//   * an existing file is never truncated or replaced by defaults;
//   * an unparsable file is copied aside before anything else happens;
//   * every write goes to a temp file and is renamed into place, so a crash
//     mid-write cannot leave a half-written config behind.
class ConfigManager {
public:
    static ConfigManager& get() {
        static ConfigManager instance;
        return instance;
    }

    void load();
    void save();
    void flush() {}
    void setModulesInitialized(bool value) { mModulesReady = value; }

    std::string getConfigPath() const;
    void setConfigPath(const std::string& path);

private:
    ConfigManager() = default;

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    static nlohmann::json defaultsJson();
    static bool writeAtomically(const std::string& path, const std::string& payload);
    static void log(const std::string& line);

    mutable std::recursive_mutex m_mutex;
    std::string m_configPath;
    bool mModulesReady = false;
};

}
