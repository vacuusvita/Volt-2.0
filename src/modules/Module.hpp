#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Base class for every Volt module.
//
// Lifecycle:
//   onInit()    once, after signatures are resolved. Installs hooks. Detours
//               must be no-ops while `enabled` is false.
//   onEnable()  every time the module becomes active.
//   onDisable() every time it stops being active.
//   onFrame()   once per rendered frame while active.
//
// `enabled` is derived from `masterEnabled && keybindActive`; nothing else can
// flip it, so a module can never end up half-active after a config load.
class Module {
public:
    const char* name;
    const char* description;
    std::string moduleId;
    bool masterEnabled = false;
    bool keybindActive = true;
    bool enabled = false;
    bool showInMenu = true;
    bool hideInHudEditor = false;
    int keybind = 0;

    Module(const char* n, const char* d) : name(n), description(d), moduleId(std::string("volt.") + n) {}
    virtual ~Module() = default;

    virtual void onInit() {}
    virtual void onEnable() {}
    virtual void onDisable() {}
    virtual void onFrame() {}
    virtual bool onMouseEvent(int, bool) { return false; }
    virtual bool onMenuConfigChanged(std::string_view, std::string_view) { return false; }
    virtual bool showInLegacyMenu(std::string_view) const { return true; }
    virtual void onMenuRegistered() {}

    virtual void onKeybindEvent(const std::string& key, bool isDown) {
        if (key == "keybind" && isDown) setKeybindActive(!keybindActive);
    }

    bool initialized() const { return mInitialized; }

    void initialize() {
        if (mInitialized) return;
        mInitialized = true;
        // Install detours only. Nothing becomes active until the config has
        // been read and resumeAll() runs, so a module that the player turned
        // off never writes a file or flips a renderer flag at startup.
        enabled = false;
        onInit();
    }

    void setMasterEnabled(bool state) {
        if (masterEnabled == state) return;
        masterEnabled = state;
        updateEnabledState();
    }

    void setKeybindActive(bool state) {
        if (keybindActive == state) return;
        keybindActive = state;
        updateEnabledState();
    }

    // Runtime-only suspend/resume used by the mod lifecycle. The config is
    // never touched: masterEnabled keeps the user's choice, while `enabled`
    // follows the mod being active in this process.
    void suspendRuntime() {
        if (!enabled) return;
        enabled = false;
        onDisable();
    }

    void resumeRuntime() {
        const bool next = masterEnabled && keybindActive;
        if (next == enabled) return;
        enabled = next;
        if (enabled) onEnable();
    }

    void updateEnabledState() {
        const bool next = masterEnabled && keybindActive;
        if (next == enabled) return;
        enabled = next;
        if (enabled) onEnable();
        else onDisable();
    }

    virtual void loadConfig(const nlohmann::json& j) {
        if (j.contains("keybind")) keybind = j["keybind"].get<int>();
        if (j.contains("keybindActive")) keybindActive = j["keybindActive"].get<bool>();
        if (j.contains("masterEnabled")) masterEnabled = j["masterEnabled"].get<bool>();
        updateEnabledState();
    }

    virtual void saveConfig(nlohmann::json& j) {
        j["keybind"] = keybind;
        j["keybindActive"] = keybindActive;
        j["masterEnabled"] = masterEnabled;
    }

private:
    bool mInitialized = false;
};
