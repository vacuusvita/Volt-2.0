#pragma once

#include "modules/Module.hpp"

// GPU Cuts writes the renderer options that the game itself exposes. It never
// patches code and never touches a material, so the worst case for a version
// mismatch is that an option name is ignored.
class GpuCutsModule : public Module {
public:
    GpuCutsModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool cutClouds = true;
    bool cutSmoothLighting = true;
    bool cutViewBobbing = true;
    bool cutFancySkies = true;
    bool cutUpscaling = false;
    bool cutVsync = false;
    bool cutParticles = true;
    bool cutDamageBobbing = true;
    int renderDistance = 0; // 0 = keep the game's value, else 4..32 chunks

private:
    bool m_ready = false;
};
