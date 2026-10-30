#include "EntityCull.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <time.h>
#include <unordered_map>

namespace {

#define VOLT_CULL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "VoltEntityCull", __VA_ARGS__)

constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/entitycull_diag.log";

void diag(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (FILE* file = std::fopen(kDiagPath, "a")) {
        std::fputs(line, file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

// ---- 1.26.60 layout -------------------------------------------------------------------------
// The Actor component-pointer block sits at: categories 0x220, StateVectorComponent 0x228,
// AABBShapeComponent 0x230, ActorRotationComponent 0x238 (1.26.52 values were +0x20 lower).
// Verified in libminecraftpe 1.26.60: Actor::getPos 0xfd8e62c reads [x0,#0x228]; the AABB
// reader at 0xfd8e63c reads [x0,#0x230]; Actor::getRot 0xfd869b0 reads [x0,#0x238].
// The actor filter at 0xb7f7d48 loads x21 from functor+0x20 and reads the distance camera there.
constexpr std::size_t kFunctorTargets = 0x10;    // filter functor -> { Actor* cameraTarget; Actor* localPlayer; ... }
constexpr std::size_t kFunctorCamera = 0x20;     // filter functor -> LevelRendererCamera*
constexpr std::size_t kCameraPosDefault = 0x620; // LRC: camera position used for actor distance
constexpr std::size_t kCameraRegion = 0x960;     // LRC: BlockSource* of the rendered dimension
constexpr std::size_t kActorPosPtr = 0x228;      // Actor -> StateVectorComponent (Vec3 pos at +0)
constexpr std::size_t kActorAabbPtr = 0x230;     // Actor -> AABBShapeComponent (AABB at +0)
constexpr std::size_t kActorCategories = 0x220;  // ActorCategory bit set (Player = 1)
constexpr std::size_t kBlockActorPosSlotDefault = 2; // renderable iface: const BlockPos& getPosition()
constexpr std::uint32_t kCategoryPlayer = 1;

std::size_t g_cameraPos = kCameraPosDefault;
std::size_t g_blockPosSlot = kBlockActorPosSlotDefault;

struct Vec3f {
    float x;
    float y;
    float z;
};

struct Aabb {
    Vec3f min;
    Vec3f max;
};

using FilterFn = void (*)(void*, void*);
FilterFn s_actorOrig = nullptr;
bedrocktools::hooks::Handle s_actorHook = nullptr;

using BlockFilterFn = void (*)(void*, void*, void*);
BlockFilterFn s_blockOrig = nullptr;
bedrocktools::hooks::Handle s_blockHook = nullptr;

struct BlockPosI {
    int x;
    int y;
    int z;
};
// BlockSource::isSolidBlockingBlock(const BlockPos&) — resolved by signature, called directly.
using IsSolidFn = bool (*)(void*, const BlockPosI*);
IsSolidFn s_isSolid = nullptr;
using BlockPosFn = const int* (*)(void*);

EntityCullModule* g_cull = nullptr;

struct CacheEntry {
    std::uint64_t checkedMs = 0;
    std::uint64_t seenMs = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool occluded = false;
};

std::mutex g_mutex;
std::unordered_map<const void*, CacheEntry> g_cache;
std::atomic<unsigned> g_frame{0};
unsigned g_budgetFrame = ~0u;
int g_budgetLeft = 0;
std::uint64_t g_lastPrune = 0;

// Liveness counters. Cheap relaxed atomics; only read by the periodic diag line.
std::atomic<unsigned> g_actorCalls{0};
std::atomic<unsigned> g_actorCulled{0};
std::atomic<unsigned> g_blockCalls{0};
std::atomic<unsigned> g_blockCulled{0};

bool looksLikePointer(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v > 0x10000 && (v & 7) == 0;
}

bool finiteCoord(float v) {
    return std::isfinite(v) && v > -3.0e7f && v < 3.0e7f;
}

std::uint64_t nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec / 1000000);
}

template <class T>
T readAt(const void* base, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
}

bool cameraPos(const void* lrc, Vec3f& out) {
    if (!looksLikePointer(lrc)) return false;
    out = readAt<Vec3f>(lrc, g_cameraPos);
    return finiteCoord(out.x) && finiteCoord(out.y) && finiteCoord(out.z);
}

void* regionOf(const void* lrc) {
    if (!looksLikePointer(lrc)) return nullptr;
    void* region = readAt<void*>(lrc, kCameraRegion);
    if (!looksLikePointer(region)) return nullptr;
    void** vtable = *reinterpret_cast<void***>(region);
    if (!looksLikePointer(vtable)) return nullptr;
    return region;
}

bool solidAt(void* region, int x, int y, int z) {
    const BlockPosI pos{x, y, z};
    return s_isSolid(region, &pos);
}

// Amanatides & Woo voxel walk from a to b. True when a solid-blocking block lies strictly between
// the voxel of a and the voxel of b (both endpoints are ignored so an actor standing in a slab or
// a camera pressed into a wall never counts as blocked).
bool rayBlocked(void* region, const Vec3f& a, const Vec3f& b, int maxSteps) {
    int x = static_cast<int>(std::floor(a.x));
    int y = static_cast<int>(std::floor(a.y));
    int z = static_cast<int>(std::floor(a.z));
    const int ex = static_cast<int>(std::floor(b.x));
    const int ey = static_cast<int>(std::floor(b.y));
    const int ez = static_cast<int>(std::floor(b.z));
    if (x == ex && y == ey && z == ez) return false;

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    constexpr float kInf = std::numeric_limits<float>::infinity();

    const int sx = dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0);
    const int sy = dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0);
    const int sz = dz > 0.0f ? 1 : (dz < 0.0f ? -1 : 0);
    const float tdx = sx != 0 ? 1.0f / std::fabs(dx) : kInf;
    const float tdy = sy != 0 ? 1.0f / std::fabs(dy) : kInf;
    const float tdz = sz != 0 ? 1.0f / std::fabs(dz) : kInf;
    float tx = sx > 0 ? (static_cast<float>(x + 1) - a.x) * tdx : (sx < 0 ? (a.x - static_cast<float>(x)) * tdx : kInf);
    float ty = sy > 0 ? (static_cast<float>(y + 1) - a.y) * tdy : (sy < 0 ? (a.y - static_cast<float>(y)) * tdy : kInf);
    float tz = sz > 0 ? (static_cast<float>(z + 1) - a.z) * tdz : (sz < 0 ? (a.z - static_cast<float>(z)) * tdz : kInf);

    for (int i = 0; i < maxSteps; ++i) {
        float t;
        if (tx < ty) {
            if (tx < tz) { x += sx; t = tx; tx += tdx; }
            else { z += sz; t = tz; tz += tdz; }
        } else {
            if (ty < tz) { y += sy; t = ty; ty += tdy; }
            else { z += sz; t = tz; tz += tdz; }
        }
        if (t >= 1.0f) return false;
        if (x == ex && y == ey && z == ez) return false;
        if (solidAt(region, x, y, z)) return true;
    }
    return false;
}

bool spendBudget() {
    const unsigned frame = g_frame.load(std::memory_order_relaxed);
    if (frame != g_budgetFrame) {
        g_budgetFrame = frame;
        g_budgetLeft = g_cull ? std::clamp(g_cull->checksPerFrame, 1, 200) : 0;
    }
    if (g_budgetLeft <= 0) return false;
    --g_budgetLeft;
    return true;
}

void pruneCache(std::uint64_t now) {
    if (now - g_lastPrune < 2000) return;
    g_lastPrune = now;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (now - it->second.seenMs > 3000) it = g_cache.erase(it);
        else ++it;
    }
}

// Shared cached occlusion query. `points` are the ray targets; occluded only if every ray is blocked.
bool occludedCached(const void* key, void* region, const Vec3f& cam, const Vec3f* points, int count, float moveTolerance) {
    if (!g_cull || count <= 0 || !s_isSolid) return false;
    const std::uint64_t now = nowMs();
    const std::uint64_t ttl = static_cast<std::uint64_t>(std::clamp(g_cull->recheckMs, 30, 2000));

    std::lock_guard<std::mutex> lock(g_mutex);
    pruneCache(now);
    auto& entry = g_cache[key];
    entry.seenMs = now;
    const float mx = points[0].x - entry.x;
    const float my = points[0].y - entry.y;
    const float mz = points[0].z - entry.z;
    const bool moved = (mx * mx + my * my + mz * mz) > moveTolerance * moveTolerance;
    const bool fresh = entry.checkedMs != 0 && (now - entry.checkedMs) < ttl && !moved;
    if (fresh) return entry.occluded;
    if (!spendBudget()) return entry.checkedMs != 0 ? entry.occluded : false;

    const int range = std::max(g_cull->actorRange, g_cull->blockActorRange);
    const int maxSteps = 3 * std::clamp(range, 16, 256) + 8;
    bool blocked = true;
    for (int i = 0; i < count && blocked; ++i) {
        blocked = rayBlocked(region, cam, points[i], maxSteps);
    }
    entry.checkedMs = now;
    entry.x = points[0].x;
    entry.y = points[0].y;
    entry.z = points[0].z;
    entry.occluded = blocked;
    return blocked;
}

bool actorPosition(const void* actor, Vec3f& out) {
    const void* state = readAt<const void*>(actor, kActorPosPtr);
    if (!looksLikePointer(state)) return false;
    out = readAt<Vec3f>(state, 0);
    return finiteCoord(out.x) && finiteCoord(out.y) && finiteCoord(out.z);
}

bool actorBounds(const void* actor, const Vec3f& pos, Aabb& out) {
    const void* shape = readAt<const void*>(actor, kActorAabbPtr);
    if (looksLikePointer(shape)) {
        out = readAt<Aabb>(shape, 0);
        const float w = out.max.x - out.min.x;
        const float h = out.max.y - out.min.y;
        const float d = out.max.z - out.min.z;
        if (finiteCoord(out.min.x) && finiteCoord(out.min.y) && finiteCoord(out.min.z) &&
            finiteCoord(out.max.x) && finiteCoord(out.max.y) && finiteCoord(out.max.z) &&
            w >= 0.0f && h >= 0.0f && d >= 0.0f && w < 64.0f && h < 64.0f && d < 64.0f &&
            std::fabs((out.min.x + out.max.x) * 0.5f - pos.x) < 8.0f &&
            std::fabs((out.min.z + out.max.z) * 0.5f - pos.z) < 8.0f) {
            return true;
        }
    }
    out.min = {pos.x - 0.4f, pos.y, pos.z - 0.4f};
    out.max = {pos.x + 0.4f, pos.y + 1.8f, pos.z + 0.4f};
    return true;
}

bool shouldCullActor(void* functor, void* actor) {
    if (!g_cull || !g_cull->enabled || !looksLikePointer(functor) || !looksLikePointer(actor)) return false;

    const void* targets = readAt<const void*>(functor, kFunctorTargets);
    if (looksLikePointer(targets)) {
        if (readAt<const void*>(targets, 0) == actor) return false;   // camera target
        if (readAt<const void*>(targets, 8) == actor) return false;   // local player
    }
    const bool isPlayer = (readAt<std::uint32_t>(actor, kActorCategories) & kCategoryPlayer) != 0;
    if (isPlayer && !g_cull->cullPlayers) return false;

    const void* lrc = readAt<const void*>(functor, kFunctorCamera);
    Vec3f cam{};
    Vec3f pos{};
    if (!cameraPos(lrc, cam) || !actorPosition(actor, pos)) return false;

    const float dx = pos.x - cam.x;
    const float dy = pos.y - cam.y;
    const float dz = pos.z - cam.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    const float near = static_cast<float>(std::clamp(g_cull->nearBlocks, 1, 64));
    if (dist2 <= near * near) return false;

    // Cave mobs under the player: XZ-only range left them queued. Keep a 3-block
    // shaft so looking down a hole still shows what's in it.
    const int below = std::clamp(g_cull->belowBlocks, 1, 64);
    if (dy < -static_cast<float>(below) && (dx * dx + dz * dz) > 9.0f) return true;

    if (g_cull->actorRange > 0) {
        const float range = static_cast<float>(g_cull->actorRange);
        if (dist2 > range * range) return true;
    }
    if (!g_cull->occlusion) return false;

    void* region = regionOf(lrc);
    if (!region) return false;

    Aabb box{};
    actorBounds(actor, pos, box);
    const float cx = (box.min.x + box.max.x) * 0.5f;
    const float cz = (box.min.z + box.max.z) * 0.5f;
    const float h = box.max.y - box.min.y;
    const float pad = std::min(0.15f, h * 0.25f);
    Vec3f points[3] = {
        {cx, (box.min.y + box.max.y) * 0.5f, cz},
        {cx, box.max.y - pad, cz},
        {cx, box.min.y + pad, cz},
    };
    return occludedCached(actor, region, cam, points, 3, 1.0f);
}

void actorFilterDetour(void* functor, void* actor) {
    if (!s_actorOrig) return;
    g_actorCalls.fetch_add(1, std::memory_order_relaxed);
    if (shouldCullActor(functor, actor)) {
        g_actorCulled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_actorOrig(functor, actor);
}

bool shouldCullBlockActor(void* lrc, void* renderable) {
    if (!g_cull || !g_cull->enabled || !g_cull->blockActors || !looksLikePointer(lrc) || !looksLikePointer(renderable)) return false;
    void** vtable = *reinterpret_cast<void***>(renderable);
    if (!looksLikePointer(vtable)) return false;
    auto posFn = reinterpret_cast<BlockPosFn>(vtable[g_blockPosSlot]);
    if (!posFn) return false;
    const int* bp = posFn(renderable);
    if (!bp) return false;
    const int bx = bp[0];
    const int by = bp[1];
    const int bz = bp[2];
    if (by < -512 || by > 512 || bx < -30000000 || bx > 30000000 || bz < -30000000 || bz > 30000000) return false;

    Vec3f cam{};
    if (!cameraPos(lrc, cam)) return false;
    const float dx = static_cast<float>(bx) + 0.5f - cam.x;
    const float dy = static_cast<float>(by) + 0.5f - cam.y;
    const float dz = static_cast<float>(bz) + 0.5f - cam.z;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    const float near = static_cast<float>(std::clamp(g_cull->nearBlocks, 1, 64));
    if (dist2 <= near * near) return false;
    if (g_cull->blockActorRange > 0) {
        const float range = static_cast<float>(g_cull->blockActorRange);
        if (dist2 > range * range) return true;
    }
    if (!g_cull->occlusion) return false;
    void* region = regionOf(lrc);
    if (!region) return false;
    const float fx = static_cast<float>(bx) + 0.5f;
    const float fz = static_cast<float>(bz) + 0.5f;
    Vec3f points[2] = {
        {fx, static_cast<float>(by) + 0.5f, fz},
        {fx, static_cast<float>(by) + 1.2f, fz},
    };
    return occludedCached(renderable, region, cam, points, 2, 0.5f);
}

void blockFilterDetour(void* lrc, void* renderable, void* tree) {
    if (!s_blockOrig) return;
    g_blockCalls.fetch_add(1, std::memory_order_relaxed);
    if (shouldCullBlockActor(lrc, renderable)) {
        g_blockCulled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s_blockOrig(lrc, renderable, tree);
}

}

EntityCullModule::EntityCullModule()
    : Module("Entity Cull", "Never queue distant mobs, items or block entities for rendering. Optional voxel-ray occlusion also hides anything behind terrain. Entities still tick and remain in the world.") {
    g_cull = this;
    moduleId = "volt.entitycull";
    masterEnabled = true;
    updateEnabledState();
}

EntityCullModule::~EntityCullModule() {
    if (g_cull == this) g_cull = nullptr;
}

void EntityCullModule::onInit() {
    // Occlusion is optional. Distance culling still works if this signature is absent.
    std::uintptr_t solidAddr = 0;
    if (!s_isSolid) {
        solidAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
        if (solidAddr) s_isSolid = reinterpret_cast<IsSolidFn>(solidAddr);
    }
    std::uintptr_t actorAddr = 0;
    if (!s_actorHook) {
        actorAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::QueueRenderActorFilter);
        if (actorAddr) {
            // First `ldr s0, [x21, #imm]` in the lambda = camera position field on the LRC (0x628 on 1.26.52).
            const auto* code = reinterpret_cast<const std::uint32_t*>(actorAddr);
            for (int i = 0; i < 0x140 / 4; ++i) {
                if ((code[i] & 0xFFC003FFu) == 0xBD4002A0u) {
                    const std::size_t imm = ((code[i] >> 10) & 0xFFFu) * 4;
                    if (imm >= 0x400 && imm <= 0x1000) g_cameraPos = imm;
                    break;
                }
            }
            s_actorHook = bedrocktools::hooks::install(reinterpret_cast<void*>(actorAddr), reinterpret_cast<void*>(actorFilterDetour), reinterpret_cast<void**>(&s_actorOrig));
        }
    }
    std::uintptr_t blockAddr = 0;
    if (!s_blockHook) {
        blockAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderBlockActorFilter);
        if (blockAddr) {
            // `ldr x8, [x8, #imm]` right before the first blr = renderable::getPosition slot.
            const auto* code = reinterpret_cast<const std::uint32_t*>(blockAddr);
            bool ok = false;
            for (int i = 0; i < 0x50 / 4; ++i) {
                if ((code[i] & 0xFFC003FFu) == 0xF9400108u) {
                    const std::size_t slot = (code[i] >> 10) & 0xFFFu;
                    if (slot < 8) {
                        g_blockPosSlot = slot;
                        ok = true;
                    }
                    break;
                }
            }
            if (ok) {
                s_blockHook = bedrocktools::hooks::install(reinterpret_cast<void*>(blockAddr), reinterpret_cast<void*>(blockFilterDetour), reinterpret_cast<void**>(&s_blockOrig));
            }
        }
    }
    m_hooked = s_actorHook != nullptr || s_blockHook != nullptr;
    VOLT_CULL_LOG("init: actorHook=%d blockHook=%d isSolid=%d camPos=0x%zx blockSlot=%zu hooked=%d",
                  s_actorHook ? 1 : 0, s_blockHook ? 1 : 0, s_isSolid ? 1 : 0,
                  g_cameraPos, g_blockPosSlot, m_hooked ? 1 : 0);
    diag("init: sig QueueRenderActorFilter=%p RenderBlockActorFilter=%p IsSolidBlocking=%p",
         (void*)actorAddr, (void*)blockAddr, (void*)solidAddr);
    diag("init: actorHook=%d blockHook=%d isSolid=%d camPos=0x%zx blockSlot=%zu hooked=%d",
         s_actorHook ? 1 : 0, s_blockHook ? 1 : 0, s_isSolid ? 1 : 0,
         g_cameraPos, g_blockPosSlot, m_hooked ? 1 : 0);
    diag("config: enabled=%d actorRange=%d blockActorRange=%d nearBlocks=%d belowBlocks=%d occlusion=%d blockActors=%d checksPerFrame=%d",
         enabled ? 1 : 0, actorRange, blockActorRange, nearBlocks, belowBlocks,
         occlusion ? 1 : 0, blockActors ? 1 : 0, checksPerFrame);
}

void EntityCullModule::onEnable() {
    if (!m_hooked) onInit();
}

void EntityCullModule::onDisable() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
}

void EntityCullModule::onFrame() {
    const unsigned frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    // ~every 5 s at 60 fps: prove the filter hooks are firing and how much they cull.
    if ((frame % 300u) == 0u) {
        std::size_t cacheSize = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cacheSize = g_cache.size();
        }
        diag("frame=%u actorCalls=%u actorCulled=%u blockCalls=%u blockCulled=%u cache=%zu",
             frame,
             g_actorCalls.load(std::memory_order_relaxed),
             g_actorCulled.load(std::memory_order_relaxed),
             g_blockCalls.load(std::memory_order_relaxed),
             g_blockCulled.load(std::memory_order_relaxed),
             cacheSize);
    }
}

void EntityCullModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("actorRange")) actorRange = j["actorRange"].get<int>();
    if (j.contains("nearBlocks")) nearBlocks = j["nearBlocks"].get<int>();
    if (j.contains("belowBlocks")) belowBlocks = j["belowBlocks"].get<int>();
    if (j.contains("occlusion")) occlusion = j["occlusion"].get<bool>();
    if (j.contains("cullPlayers")) cullPlayers = j["cullPlayers"].get<bool>();
    if (j.contains("recheckMs")) recheckMs = j["recheckMs"].get<int>();
    if (j.contains("checksPerFrame")) checksPerFrame = j["checksPerFrame"].get<int>();
    if (j.contains("blockActors")) blockActors = j["blockActors"].get<bool>();
    if (j.contains("blockActorRange")) blockActorRange = j["blockActorRange"].get<int>();
    actorRange = std::clamp(actorRange, 0, 200);
    nearBlocks = std::clamp(nearBlocks, 1, 64);
    belowBlocks = std::clamp(belowBlocks, 1, 64);
    recheckMs = std::clamp(recheckMs, 30, 2000);
    checksPerFrame = std::clamp(checksPerFrame, 1, 200);
    blockActorRange = std::clamp(blockActorRange, 0, 200);
    diag("config loaded: enabled=%d actorRange=%d blockActorRange=%d nearBlocks=%d belowBlocks=%d occlusion=%d blockActors=%d checksPerFrame=%d recheckMs=%d",
         enabled ? 1 : 0, actorRange, blockActorRange, nearBlocks, belowBlocks,
         occlusion ? 1 : 0, blockActors ? 1 : 0, checksPerFrame, recheckMs);
}

void EntityCullModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["actorRange"] = actorRange;
    j["nearBlocks"] = nearBlocks;
    j["belowBlocks"] = belowBlocks;
    j["occlusion"] = occlusion;
    j["cullPlayers"] = cullPlayers;
    j["recheckMs"] = recheckMs;
    j["checksPerFrame"] = checksPerFrame;
    j["blockActors"] = blockActors;
    j["blockActorRange"] = blockActorRange;
}
