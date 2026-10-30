#pragma once

#include "modules/Module.hpp"

// Fix SpikeLag spreads chunk mesh builds over frames instead of letting the
// engine queue a whole burst at once.
//
// It sits behind Heavy Cull on LevelBuilder::scheduleChunkBuild, so chunks
// Heavy Cull already refused never spend budget. Pacing only starts after a
// warm-up of successful calls, which keeps world load and teleports at vanilla
// speed, and it only runs while the camera is actually moving.
class FixSpikeLagModule : public Module {
public:
    FixSpikeLagModule();
    ~FixSpikeLagModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int maxBuilds = 8;            // builds allowed per window; 0 disables pacing
    int windowMs = 16;            // window length in milliseconds
    bool onlyWhileMoving = true;  // keep vanilla speed while standing still
    bool adaptive = true;         // tighten briefly after a frame spike
    int spikeMs = 50;
    int cooldownFrames = 120;

private:
    bool m_hooked = false;
};
