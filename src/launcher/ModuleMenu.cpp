#include "ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace pl::modmenu {
// The V2 config schema API is newer than the base registration API, so it is
// referenced weakly: on a preloader that does not export the symbol the
// reference resolves to null and the schema is simply skipped instead of
// failing the library load.
PL_EXPORT bool setConfigSchemaJson(std::string_view moduleId, std::string_view schemaJson)
        __attribute__((weak));
}

namespace {

using pl::modmenu::ConfigType;

// One menu entry, mirroring the config keys a module saves.
struct ConfigEntryInfo {
    std::string key;
    std::string displayName;
    ConfigType type;
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string dependsOn;
};

// Short help text shown under a setting in the mod menu, keyed by module name
// (as registered) and config key. Keep each line short: the menu renders it as
// small secondary text right under the control title.
struct SettingHelp {
    std::string_view module;
    std::string_view key;
    std::string_view description;
};

constexpr SettingHelp kSettingHelp[] = {
        // FPS Unlock
        {"FPS Unlock", "targetFPS",
         "Frame cap. 0 = unlimited: the game renders as fast as it can. Values like 60/90/120 hold the frame rate under that number to reduce heat and screen tearing. VSync is forced off while the module is on."},

        // GPU Cuts
        {"GPU Cuts", "cutClouds",
         "Force clouds off in the game's options. Big GPU save in open sky."},
        {"GPU Cuts", "cutSmoothLighting",
         "Force flat lighting: smooth lighting off in the game's options."},
        {"GPU Cuts", "cutViewBobbing",
         "Disable view bobbing in the game's options."},
        {"GPU Cuts", "cutFancySkies",
         "Force the simple sky: no fancy sunrise, sunset or stars."},
        {"GPU Cuts", "cutUpscaling",
         "Turn off resolution upscaling. Only helps on devices where the upscaler costs more than it saves."},
        {"GPU Cuts", "cutVsync",
         "Force VSync off. Usually redundant with FPS Unlock, which already does it."},
        {"GPU Cuts", "cutParticles",
         "Set the particle view distance to the minimum the game allows."},
        {"GPU Cuts", "cutDamageBobbing",
         "Disable the screen tilt when you take damage."},
        {"GPU Cuts", "renderDistance",
         "Force the game's render distance, in chunks. 0 = keep the game's own setting. Anything above 0 is clamped to 4-32; 2 chunks renders an empty world."},

        // Entity Cull
        {"Entity Cull", "actorRange",
         "Maximum distance in blocks for mobs, animals and dropped items to be rendered. 0 = no limit. Lower = more FPS."},
        {"Entity Cull", "blockActorRange",
         "Same as Actor Range, but for block entities: chests, signs, banners, beds, spawners. 0 = no limit."},
        {"Entity Cull", "nearBlocks",
         "Safety radius in blocks. Nothing inside it is ever hidden."},
        {"Entity Cull", "belowBlocks",
         "Entities more than this many blocks below your camera (caves) stop rendering, except a small shaft right under you."},
        {"Entity Cull", "occlusion",
         "Voxel ray test: entities completely hidden behind solid blocks stop rendering. Small CPU cost, big GPU save."},
        {"Entity Cull", "cullPlayers",
         "Also hide other players beyond the ranges. Off = other players are always visible."},
        {"Entity Cull", "blockActors",
         "Master switch for block entities (chests, signs, banners...). Off = only mobs and items are culled."},
        {"Entity Cull", "recheckMs",
         "How long (ms) each entity's hide/show decision is cached. Higher = less CPU, but reacts slower to changes."},
        {"Entity Cull", "checksPerFrame",
         "Maximum occlusion ray tests per frame. Lower = less CPU, but hiding may lag behind for a moment."},

        // Heavy Cull
        {"Heavy Cull", "cullChunks",
         "Radius in chunks that is still built and drawn. Chunks past it are never meshed. Lower = more FPS and less visible world. The chunk you stand in is never culled."},
        {"Heavy Cull", "belowBlocks",
         "Sections deeper than this many blocks under your feet stop being built. Caves far below you cost nothing."},
        {"Heavy Cull", "viewCone",
         "Field of view kept behind the camera, in degrees. 180 = off (draw all around). 90 = only what is roughly in front."},
        {"Heavy Cull", "behindCamera",
         "Drop chunks outside the view cone. The classic big win: only what the camera can see is drawn."},

        // Fix SpikeLag
        {"Fix SpikeLag", "maxBuilds",
         "Chunk mesh rebuilds allowed per window. Lower = smoother frames but slower chunk fill-in. 0 = pacing off (vanilla)."},
        {"Fix SpikeLag", "windowMs",
         "Length of the budget window in ms. Bigger = more chunk builds per burst."},
        {"Fix SpikeLag", "onlyWhileMoving",
         "Only pace while you are moving or turning the camera. Standing still keeps vanilla speed for world loading and teleports."},
        {"Fix SpikeLag", "adaptive",
         "After a frame spike, tighten the budget to one build per window for a short cool-down. A very long hitch pauses builds for a few frames."},
        {"Fix SpikeLag", "spikeMs",
         "Frame time (ms) that counts as a spike. Lower = reacts to smaller stutters."},
        {"Fix SpikeLag", "cooldownFrames",
         "How many frames to stay tightened after a spike."},

        // Ram Optimizer
        {"Ram Optimizer", "intervalSeconds",
         "How often freed memory is returned to Android, in seconds. Lower = RAM drops sooner, at a tiny CPU cost."},
        {"Ram Optimizer", "onlyWhenIdle",
         "Only purge while the frame time is calm, so the purge itself never causes a hitch."},

};

std::string_view describeSetting(std::string_view module, std::string_view key) {
    if (key == "keybind") {
        return "Key or button that toggles this module on and off while playing.";
    }
    for (const auto& entry : kSettingHelp) {
        if (entry.module == module && entry.key == key) return entry.description;
    }
    return {};
}

// Unit appended to the value in the menu (e.g. "32 blocks").
std::string_view settingUnit(std::string_view key) {
    if (key == "actorRange" || key == "blockActorRange" || key == "nearBlocks" ||
        key == "belowBlocks") {
        return "blocks";
    }
    if (key == "recheckMs" || key == "windowMs" || key == "spikeMs") return "ms";
    if (key == "intervalSeconds") return "s";
    if (key == "cullChunks" || key == "renderDistance") return "chunks";
    if (key == "viewCone") return "\u00b0";
    if (key == "targetFPS") return "fps";
    if (key == "cooldownFrames") return "frames";
    return {};
}

pl::modmenu::ConfigControlTypeV2 schemaType(ConfigType type) {
    switch (type) {
        case ConfigType::SliderInt: return pl::modmenu::ConfigControlTypeV2::SliderInt;
        case ConfigType::SliderFloat: return pl::modmenu::ConfigControlTypeV2::SliderFloat;
        case ConfigType::Radio: return pl::modmenu::ConfigControlTypeV2::Choice;
        case ConfigType::Color: return pl::modmenu::ConfigControlTypeV2::Color;
        case ConfigType::Keybind: return pl::modmenu::ConfigControlTypeV2::Keybind;
        case ConfigType::Text: return pl::modmenu::ConfigControlTypeV2::Text;
        case ConfigType::Button: return pl::modmenu::ConfigControlTypeV2::Button;
        default: return pl::modmenu::ConfigControlTypeV2::Toggle;
    }
}

std::vector<ConfigEntryInfo> buildConfigEntries(Module& mod) {
    static const char* baseKeys[] = {
            "enabled", "masterEnabled", "keybindActive", "shortcutEnabled", "shortcutSize", "shortcutOpacity",
            "lockPosition", "shortcutPosX", "shortcutPosY",
            "isHudModule", "hudPosX", "hudPosY"
    };

    nlohmann::json j;
    mod.saveConfig(j);

    std::vector<ConfigEntryInfo> configs;

    for (auto& [k, v] : j.items()) {
        bool isBase = false;
        for (const char* bk : baseKeys) {
            if (k == bk) { isBase = true; break; }
        }
        std::string keyLower = k;
        std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
        const bool isHudPosition = keyLower.rfind("hud", 0) == 0 &&
            (keyLower.ends_with("posx") || keyLower.ends_with("posy"));
        if (isBase || isHudPosition || !mod.showInLegacyMenu(k)) continue;

        std::string displayName;
        std::string sourceKey = k;
        if (sourceKey.size() > 2 && sourceKey[0] == 'm' && sourceKey[1] == '_') {
            sourceKey = sourceKey.substr(2);
        }
        for (size_t i = 0; i < sourceKey.size(); ++i) {
            if (i == 0) {
                displayName += toupper(sourceKey[i]);
            } else if (isupper(sourceKey[i])) {
                displayName += ' ';
                displayName += sourceKey[i];
            } else {
                displayName += sourceKey[i];
            }
        }

        ConfigEntryInfo entry;
        entry.key = k;
        entry.displayName = displayName;

        if (v.is_boolean()) {
            std::string kLower = k;
            std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
            if (kLower.find("button") != std::string::npos) {
                entry.type = ConfigType::Button;
            } else {
                entry.type = ConfigType::Toggle;
            }
            entry.defaultValue = v.get<bool>() ? "true" : "false";
        } else if (v.is_number_integer()) {
            entry.type = ConfigType::SliderInt;
            entry.defaultValue = std::to_string(v.get<int>());

            std::string kLower = k;
            std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

            if (kLower.find("keybind") != std::string::npos) {
                entry.type = ConfigType::Keybind;
            } else {
                int minVal = 0;
                int maxVal = 200;
                if (kLower == "intervalseconds") {
                    minVal = 5;
                    maxVal = 300;
                } else if (kLower == "maxbuilds") {
                    minVal = 0;
                    maxVal = 16;
                } else if (kLower == "windowms") {
                    minVal = 4;
                    maxVal = 64;
                } else if (kLower == "spikems") {
                    minVal = 16;
                    maxVal = 250;
                } else if (kLower == "cooldownframes") {
                    minVal = 0;
                    maxVal = 200;
                } else if (kLower.find("cps") != std::string::npos) {
                    minVal = 1;
                    maxVal = 30;
                } else if (kLower.find("every") != std::string::npos) {
                    minVal = 1;
                    maxVal = 16;
                } else if (kLower.find("perframe") != std::string::npos) {
                    minVal = 1;
                    maxVal = 200;
                } else if (kLower.find("recheck") != std::string::npos) {
                    minVal = 30;
                    maxVal = 2000;
                } else if (kLower.find("range") != std::string::npos) {
                    minVal = 0;
                    maxVal = 200;
                } else if (kLower.find("fps") != std::string::npos) {
                    minVal = 0;
                    maxVal = 120;
                } else if (kLower.find("cone") != std::string::npos) {
                    minVal = 60;
                    maxVal = 180;
                } else if (kLower.find("grass") != std::string::npos) {
                    minVal = 0;
                    maxVal = 16;
                } else if (kLower.find("entity") != std::string::npos) {
                    minVal = 0;
                    maxVal = 16;
                } else if (kLower.find("below") != std::string::npos || kLower.find("blocks") != std::string::npos) {
                    minVal = 1;
                    maxVal = 64;
                } else if (kLower == "cullchunks") {
                    // 2 chunks culls ~95% of the view area and reads as an empty world.
                    minVal = 6;
                    maxVal = 32;
                } else if (kLower.find("cull") != std::string::npos || kLower.find("chunk") != std::string::npos) {
                    minVal = 2;
                    maxVal = 16;
                } else if (kLower.find("renderdistance") != std::string::npos || kLower.find("viewdistance") != std::string::npos) {
                    minVal = 0;
                    maxVal = 32;
                } else if (kLower.find("percent") != std::string::npos) {
                    minVal = 33;
                    maxVal = 100;
                } else if (kLower.find("fog") != std::string::npos) {
                    minVal = 8;
                    maxVal = 256;
                } else if (kLower.find("time") != std::string::npos) {
                    maxVal = 24000;
                }

                entry.minValue = std::to_string(minVal);
                entry.maxValue = std::to_string(maxVal);
            }
        } else if (v.is_number_float()) {
            entry.type = ConfigType::SliderFloat;
            entry.defaultValue = std::to_string(v.get<float>());

            std::string kLower = k;
            std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

            float minVal = 0.0f;
            float maxVal = 100.0f;

            if (kLower.find("opacity") != std::string::npos ||
                kLower.find("color") != std::string::npos ||
                kLower.find("alpha") != std::string::npos) {
                maxVal = 1.0f;
            } else if (kLower.find("scale") != std::string::npos) {
                minVal = 0.1f;
                maxVal = 5.0f;
            } else if (kLower == "borderwidth") {
                maxVal = 4.0f;
            } else if (kLower.find("width") != std::string::npos) {
                maxVal = 1000.0f;
            } else if (kLower.find("position") != std::string::npos ||
                       kLower.find("posx") != std::string::npos ||
                       kLower.find("posy") != std::string::npos) {
                maxVal = 2000.0f;
            } else if (kLower.find("range") != std::string::npos) {
                maxVal = 180.0f;
            } else if (kLower.find("fov") != std::string::npos) {
                minVal = 1.0f;
                maxVal = 179.0f;
            } else if (kLower.find("intensity") != std::string::npos) {
                maxVal = 10.0f;
            } else if (kLower.find("speed") != std::string::npos || kLower.find("strength") != std::string::npos) {
                minVal = 0.05f;
                maxVal = 1.0f;
            } else if (kLower.find("thick") != std::string::npos) {
                maxVal = 20.0f;
            }

            entry.minValue = std::to_string(minVal);
            entry.maxValue = std::to_string(maxVal);
        } else if (v.is_string()) {
            std::string str = v.get<std::string>();
            if (str.find(',') != std::string::npos) {
                entry.type = ConfigType::Radio;
                size_t firstComma = str.find(',');
                entry.defaultValue = str.substr(0, firstComma);
                entry.minValue = str.substr(firstComma + 1);
            } else if (!str.empty() && str[0] == '#') {
                entry.type = ConfigType::Color;
                entry.defaultValue = str;
            } else {
                entry.type = ConfigType::Text;
                entry.defaultValue = str;
            }
        } else {
            continue;
        }
        configs.push_back(entry);
    }

    for (auto& entry : configs) {
        const std::string& k = entry.key;
        if (entry.type != ConfigType::Toggle) {
            std::string bestParent = "";
            for (const auto& parentCandidate : configs) {
                if (parentCandidate.type == ConfigType::Toggle) {
                    const std::string& pKey = parentCandidate.key;
                    if (k.length() > pKey.length() && k.compare(0, pKey.length(), pKey) == 0) {
                        if (pKey.length() > bestParent.length()) {
                            bestParent = pKey;
                        }
                    }
                }
            }
            if (!bestParent.empty()) entry.dependsOn = bestParent;
        }
    }

    return configs;
}

// Publishes the V2 config schema for a module: same entries as the legacy
// registration, plus a description (and unit) for every setting. The launcher
// menu renders the description under the control title; re-registering after a
// config change bumps the schema revision, which makes an open menu refresh.
void registerModuleSchema(Module& mod) {
    if (!pl::modmenu::setConfigSchemaJson) return;

    const auto configs = buildConfigEntries(mod);

    pl::modmenu::ConfigSchemaBuilder schema;
    schema.defaultCategory("settings");
    schema.category("settings", "Settings");

    pl::modmenu::ConfigNodeV2 about;
    about.id = "about";
    about.category = "settings";
    about.title = mod.name;
    about.description = mod.description;
    about.type = pl::modmenu::ConfigControlTypeV2::Info;
    schema.node(std::move(about));

    for (const auto& entry : configs) {
        if (entry.type == ConfigType::Button) continue;

        pl::modmenu::ConfigNodeV2 node;
        node.id = entry.key;
        node.key = entry.key;
        node.category = "settings";
        node.title = entry.displayName;
        node.description = std::string(describeSetting(mod.name, entry.key));
        node.type = schemaType(entry.type);
        node.defaultValue = entry.defaultValue;
        node.currentValue = entry.defaultValue;
        node.hasCurrentValue = true;
        node.minValue = entry.minValue;
        node.maxValue = entry.maxValue;
        node.unit = std::string(settingUnit(entry.key));
        schema.node(std::move(node));
    }

    pl::modmenu::setConfigSchemaJson(mod.moduleId, schema.toJson());
}

} // namespace

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->setMasterEnabled(enabled);
    bedrocktools::config::ConfigManager::get().save();
}

static void onModuleKeybind(std::string_view module_id, std::string_view key, bool isDown) {
    if (ModuleRegistry::get().keybindBlocked()) return;
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->onKeybindEvent(std::string(key), isDown);
}

static void onModuleConfigChanged(std::string_view module_id, std::string_view key, std::string_view value) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    if (mod->onMenuConfigChanged(key, value)) {
        bedrocktools::config::ConfigManager::get().save();
        registerModuleSchema(*mod);
        return;
    }

    nlohmann::json j;
    mod->saveConfig(j);

    std::string safeValue(value);
    std::string safeKey(key);
    try {
        if (j.contains(safeKey)) {
            if (j[safeKey].is_boolean()) {
                if (safeValue == "true") j[safeKey] = true;
                else if (safeValue == "false") j[safeKey] = false;
            } else if (j[safeKey].is_number_integer()) {
                if (!safeValue.empty()) {
                    char* end;
                    int val = std::strtol(safeValue.c_str(), &end, 10);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                }
            } else if (j[safeKey].is_number_float()) {
                if (!safeValue.empty()) {
                    char* end;
                    float val = std::strtof(safeValue.c_str(), &end);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                }
            } else {
                j[safeKey] = safeValue;
            }
        } else {
            j[safeKey] = safeValue;
        }
    } catch (...) {
        j[safeKey] = safeValue;
    }
    mod->loadConfig(j);
    bedrocktools::config::ConfigManager::get().save();
    registerModuleSchema(*mod);
}

void registerModulesWithLauncher() {
    auto& modules = ModuleRegistry::get().modules();

    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;
        try {

        pl::modmenu::ModuleBuilder builder(mod->moduleId, mod->name);
        builder.description(mod->description)
                .defaultEnabled(mod->masterEnabled)
                .hideInHudEditor(mod->hideInHudEditor)
                .onToggle(onModuleToggle)
                .onConfigChanged(onModuleConfigChanged)
                .onKeybind(onModuleKeybind);

        const auto configs = buildConfigEntries(*mod);

        for (const auto& entry : configs) {
            builder.config(entry.key, entry.displayName, entry.type, entry.defaultValue,
                           entry.minValue, entry.maxValue, entry.dependsOn);
        }

        if (builder.registerModule()) {
            mod->onMenuRegistered();
            registerModuleSchema(*mod);
        }
        } catch (...) {
        }
    }
}
