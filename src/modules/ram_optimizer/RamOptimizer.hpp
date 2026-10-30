#pragma once

#include "modules/Module.hpp"

// Returns allocator pages to the kernel while the game is idle.
//
// This module never touches the game: it only calls the C allocator once every
// N seconds, and only when the frame time is calm, so the purge itself cannot
// cause the hitch it is meant to avoid.
class RamOptimizerModule : public Module {
public:
    RamOptimizerModule();

    void onEnable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int intervalSeconds = 20;
    bool onlyWhenIdle = true;

private:
    long long m_lastPurgeMs = 0;
    long long m_lastFrameMs = 0;
    int m_calmFrames = 0;
};
