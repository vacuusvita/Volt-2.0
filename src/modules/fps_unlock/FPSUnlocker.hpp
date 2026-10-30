#pragma once

#include "modules/Module.hpp"
#include <EGL/egl.h>
#include <time.h>

class FPSUnlockerModule : public Module {
public:
    FPSUnlockerModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    int targetFPS = 0;

    struct timespec m_lastFrameTime = {};
    bool m_hasLastFrame = false;
};
