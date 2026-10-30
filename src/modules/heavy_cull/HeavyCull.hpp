#pragma once

#include "modules/Module.hpp"

// Heavy Cull limits how far terrain chunks are built and drawn.
//
// Two detours, both pass-through while the module is off:
//   * LevelBuilder::scheduleChunkBuild  - refuse meshes outside the radius
//   * LevelRendererCamera::setupViewArea - drop those chunks from the draw list
//
// The radius is expressed in chunks and clamped to a range that always leaves
// a visible world. Nothing here writes a renderer field: culling is decided
// from the chunk position and the player position only, so a layout change in
// a future build degrades to "no culling" instead of "no world".
class HeavyCullModule : public Module {
public:
    HeavyCullModule();
    ~HeavyCullModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int cullChunks = 12;   // radius in chunks that is still built and drawn
    int belowBlocks = 24;  // never build sections deeper than this below the surface
    int viewCone = 180;    // degrees kept behind the camera, 180 = off
    bool behindCamera = false;

private:
    bool m_hooked = false;
};
