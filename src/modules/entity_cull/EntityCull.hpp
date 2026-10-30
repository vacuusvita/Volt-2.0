#pragma once

#include "modules/Module.hpp"

// Entity Cull — distance and occlusion culling for actors and block entities.
//
// Hooks the per-actor filter lambda of LevelRendererCamera::queueRenderEntities and the
// block-actor filter. An early return there is exactly what the game does for actors it
// decides not to draw (outside frustum / over budget), so culled actors simply never reach
// the render queue. The entity still exists, ticks, makes sound and can be hit; only its
// rendering is suppressed.
//
// Rules (first match wins):
//   1. never cull the camera target, the local player, UI previews, or anything within nearBlocks
//   2. actors more than belowBlocks under the camera are culled (caves), except a small shaft
//   3. beyond actorRange / blockActorRange blocks -> culled
//   4. occlusion: voxel rays (centre / head / feet) from the camera to the actor AABB through
//      BlockSource::isSolidBlockingBlock. All blocked -> culled. Cached per actor for recheckMs,
//      at most checksPerFrame fresh ray casts per frame.
class EntityCullModule : public Module {
public:
    EntityCullModule();
    ~EntityCullModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int actorRange = 32;         // blocks, 0 = no hard cap
    int nearBlocks = 4;          // never cull inside this radius
    int belowBlocks = 8;         // drop actors this many blocks under the camera (caves)
    bool occlusion = true;       // voxel ray occlusion test
    bool cullPlayers = false;    // other players are exempt unless enabled
    int recheckMs = 300;         // cache lifetime per actor
    int checksPerFrame = 32;     // fresh ray-cast budget per frame
    bool blockActors = true;     // also cull block entities (chests, signs, banners...)
    int blockActorRange = 24;    // blocks, 0 = no hard cap

private:
    bool m_hooked = false;
};
