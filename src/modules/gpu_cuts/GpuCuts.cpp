#include "GpuCuts.hpp"
#include "config/OptionsFile.hpp"

#include <android/log.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/gpucuts_diag.log";

constexpr int kMinRenderDistance = 4;
constexpr int kMaxRenderDistance = 32;

void diag(const std::string& line) {
    if (FILE* file = std::fopen(kDiagPath, "a")) {
        std::fputs(line.c_str(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
    __android_log_print(ANDROID_LOG_INFO, "VoltGpuCuts", "%s", line.c_str());
}

const std::vector<std::string>& optionsCandidates() {
    static const std::vector<std::string> candidates = {
        "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/_shared/internal/games/"
        "com.mojang/minecraftpe/options.txt",
        "/sdcard/Android/media/org.levimc.launcher/minecraft/_shared/internal/games/"
        "com.mojang/minecraftpe/options.txt",
        "/data/data/com.mojang.minecraftpe/files/games/com.mojang/minecraftpe/options.txt",
    };
    return candidates;
}

std::string findOptionsPath() {
    std::error_code ec;
    for (const auto& candidate : optionsCandidates()) {
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate;
        ec.clear();
    }
    return {};
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool writeFileAtomically(const std::string& path, const std::string& payload) {
    const std::string temp = path + ".volt-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

struct BackupEntry {
    std::string original;
    std::string applied;
};

std::string backupPath() {
    return "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
           "mods/Volt/config/gfx_options_backup.json";
}

nlohmann::json loadBackup() {
    std::string text;
    if (!readFile(backupPath(), text) || text.empty()) return nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(text);
        return parsed.is_object() ? parsed : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

void saveBackup(const nlohmann::json& backup) {
    std::error_code ec;
    const std::filesystem::path path(backupPath());
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    writeFileAtomically(backupPath(), backup.dump(2));
}

}

GpuCutsModule::GpuCutsModule()
    : Module("GPU Cuts",
             "Turns off clouds, smooth lighting, view bobbing, fancy skies and particles through the game's own options.") {
    moduleId = "volt.gpucuts";
    masterEnabled = true;
    updateEnabledState();
}

void GpuCutsModule::onInit() {
    m_ready = !findOptionsPath().empty();
    diag(std::string("init options=") + (m_ready ? findOptionsPath() : "not found"));
}

void GpuCutsModule::onEnable() {
    if (!m_ready) onInit();
    const std::string path = findOptionsPath();
    if (path.empty()) {
        diag("enable: options.txt not found");
        return;
    }

    std::string text;
    if (!readFile(path, text)) {
        diag("enable: cannot read " + path);
        return;
    }

    auto values = bedrocktools::config::parseOptions(text);
    auto backup = loadBackup();
    auto& written = backup["written"];
    if (!written.is_object()) written = nlohmann::json::object();

    auto apply = [&](const char* key, const std::string& value) {
        const auto it = values.find(key);
        const std::string previous = it == values.end() ? std::string() : it->second;
        // Keep the value that existed before Volt touched the key, not the
        // value from a previous Volt run.
        if (!written.contains(key)) {
            written[key] = nlohmann::json{{"original", previous}, {"applied", value}};
        } else {
            written[key]["applied"] = value;
        }
        values[key] = value;
    };

    if (cutClouds) apply("gfx_toggleclouds", "0");
    if (cutSmoothLighting) apply("gfx_smoothlighting", "0");
    if (cutViewBobbing) apply("gfx_viewbobbing", "0");
    if (cutFancySkies) apply("gfx_fancyskies", "0");
    if (cutUpscaling) apply("gfx_upscaling", "0");
    if (cutVsync) apply("gfx_vsync", "0");
    if (cutParticles) {
        apply("gfx_particleviewdistance", "0");
        apply("gfx_bubble_particles", "0");
    }
    if (cutDamageBobbing) apply("gfx_damagebobbing", "0");
    if (renderDistance > 0) {
        const int chunks = std::clamp(renderDistance, kMinRenderDistance, kMaxRenderDistance);
        apply("gfx_viewdistance", std::to_string(chunks * 16));
    }

    const std::string output = bedrocktools::config::serializeOptions(text, values);
    if (!writeFileAtomically(path, output)) {
        diag("enable: cannot write " + path);
        return;
    }
    saveBackup(backup);
    diag("enable: applied " + std::to_string(written.size()) + " option(s) to " + path);
}

void GpuCutsModule::onDisable() {
    const std::string path = findOptionsPath();
    if (path.empty()) return;
    std::string text;
    if (!readFile(path, text)) return;

    auto values = bedrocktools::config::parseOptions(text);
    auto backup = loadBackup();
    if (!backup.contains("written") || !backup["written"].is_object()) return;
    auto& written = backup["written"];

    int restored = 0;
    for (auto it = written.begin(); it != written.end(); ++it) {
        const std::string key = it.key();
        const std::string applied = it.value().value("applied", std::string());
        const std::string original = it.value().value("original", std::string());
        const auto current = values.find(key);
        // Only restore when the file still holds the value Volt wrote. If the
        // player changed it in the settings screen, that value wins.
        if (current == values.end() || current->second != applied) continue;
        if (original.empty()) values.erase(key);
        else values[key] = original;
        ++restored;
    }

    if (restored > 0 && writeFileAtomically(path, bedrocktools::config::serializeOptions(text, values))) {
        diag("disable: restored " + std::to_string(restored) + " option(s)");
    }
    saveBackup(nlohmann::json::object());
}

void GpuCutsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto readBool = [&](const char* key, bool& target) {
        if (j.contains(key)) target = j[key].get<bool>();
    };
    readBool("cutClouds", cutClouds);
    readBool("cutSmoothLighting", cutSmoothLighting);
    readBool("cutViewBobbing", cutViewBobbing);
    readBool("cutFancySkies", cutFancySkies);
    readBool("cutUpscaling", cutUpscaling);
    readBool("cutVsync", cutVsync);
    readBool("cutParticles", cutParticles);
    readBool("cutDamageBobbing", cutDamageBobbing);
    if (j.contains("renderDistance")) renderDistance = j["renderDistance"].get<int>();
    if (renderDistance < 0) renderDistance = 0;
    if (renderDistance > 0) renderDistance = std::clamp(renderDistance, kMinRenderDistance, kMaxRenderDistance);
}

void GpuCutsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["cutClouds"] = cutClouds;
    j["cutSmoothLighting"] = cutSmoothLighting;
    j["cutViewBobbing"] = cutViewBobbing;
    j["cutFancySkies"] = cutFancySkies;
    j["cutUpscaling"] = cutUpscaling;
    j["cutVsync"] = cutVsync;
    j["cutParticles"] = cutParticles;
    j["cutDamageBobbing"] = cutDamageBobbing;
    j["renderDistance"] = renderDistance;
}
