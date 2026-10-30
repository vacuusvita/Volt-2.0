#include "HeavyCull.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t kInstancedSharedPtr = 0x10;
constexpr std::size_t kSharedBlockPos = 0x14;
constexpr int kMinCullChunks = 6;
constexpr int kMaxCullChunks = 32;
constexpr int kMinViewEntries = 8;   // below this the span is not a chunk list
constexpr std::size_t kMaxSpanBytes = 16ull * 65536;
constexpr int kRebuildFrames = 120;
constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/heavy_diag.log";

HeavyCullModule* g_cull = nullptr;
std::atomic<int> g_rebuildFrames{0};
std::atomic<int> g_viewPasses{0};
std::atomic<int> g_logBudget{6};

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

bool looksLikePointer(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v > 0x10000 && (v & 7) == 0;
}

bool finiteCoord(float v) {
    return std::isfinite(v) && v > -3.0e7f && v < 3.0e7f;
}

bool chunkPos(void* instanced, int& bx, int& by, int& bz) {
    if (!looksLikePointer(instanced)) return false;
    auto* shared = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(instanced) + kInstancedSharedPtr);
    if (!looksLikePointer(shared)) return false;
    const auto* pos = reinterpret_cast<const int*>(static_cast<std::uint8_t*>(shared) + kSharedBlockPos);
    bx = pos[0];
    by = pos[1];
    bz = pos[2];
    if ((bx & 15) != 0 || (bz & 15) != 0) return false;
    if (by < -2048 || by > 2048) return false;
    return true;
}

// Player data published by the tick event. The camera argument of the detours
// is not trusted for coordinates: on 1.26.60 the field offsets are unverified
// and a wrong read would make every chunk look far away.
float g_playerX = 0.0f;
float g_playerY = 64.0f;
float g_playerZ = 0.0f;
float g_fwdX = 0.0f;
float g_fwdZ = 1.0f;
bool g_havePlayer = false;

bool shouldCullChunk(void* instanced, bool gpu, int* reason = nullptr) {
    if (reason) *reason = 0;
    if (!g_cull || !g_cull->enabled) return false;
    if (g_rebuildFrames.load(std::memory_order_acquire) > 0) return false;
    if (!g_havePlayer) return false;

    int bx = 0;
    int by = 0;
    int bz = 0;
    if (!chunkPos(instanced, bx, by, bz)) return false;

    const float chunkX = static_cast<float>(bx) + 8.0f;
    const float chunkZ = static_cast<float>(bz) + 8.0f;
    const float dx = chunkX - g_playerX;
    const float dz = chunkZ - g_playerZ;
    const float mag = std::sqrt(dx * dx + dz * dz);
    const float distChunks = mag * (1.0f / 16.0f);

    // The chunk the player stands in is never culled, whatever the settings.
    if (distChunks <= 1.0f) return false;

    const int py = static_cast<int>(g_playerY);
    int radius = std::clamp(g_cull->cullChunks, kMinCullChunks, kMaxCullChunks);
    if (distChunks > static_cast<float>(radius)) {
        if (reason) *reason = 1;
        return true;
    }

    if (gpu && g_cull->behindCamera && mag > 8.0f) {
        const float hlen = std::sqrt(g_fwdX * g_fwdX + g_fwdZ * g_fwdZ);
        if (hlen > 0.15f) {
            const float dot = (dx / mag) * (g_fwdX / hlen) + (dz / mag) * (g_fwdZ / hlen);
            const float chunkHalf = std::atan2(12.0f, mag);
            float half = static_cast<float>(std::clamp(g_cull->viewCone, 60, 180)) * 0.008726646f + chunkHalf;
            if (half > 1.5707963f) half = 1.5707963f;
            if (dot < std::cos(half)) {
                if (reason) *reason = 2;
                return true;
            }
        }
    }

    if ((by & 15) == 0) {
        const int floorBlocks = std::clamp(g_cull->belowBlocks, 1, 64);
        if (by + 16 < py - floorBlocks) {
            if (reason) *reason = 4;
            return true;
        }
    }
    return false;
}

using ScheduleChunkBuildFn = bool (*)(void*, void*, void*, void*, void*);
ScheduleChunkBuildFn s_scheduleOrig = nullptr;
bedrocktools::hooks::Handle s_scheduleHook = nullptr;

bool scheduleChunkBuildDetour(void* builder, void* instanced, void* sharedPtr,
                              void* levelChunk, void* unused) {
    if (!s_scheduleOrig) return false;
    if (!g_cull || !g_cull->enabled) {
        return s_scheduleOrig(builder, instanced, sharedPtr, levelChunk, unused);
    }
    if (shouldCullChunk(instanced, false)) return false;
    return s_scheduleOrig(builder, instanced, sharedPtr, levelChunk, unused);
}

struct ChunkSpan {
    std::uint8_t* begin;
    std::uint8_t* end;
};
using SetupViewAreaFn = void (*)(void*, ChunkSpan*);
SetupViewAreaFn s_viewOrig = nullptr;
bedrocktools::hooks::Handle s_viewHook = nullptr;

void setupViewAreaDetour(void* lambda, ChunkSpan* span) {
    if (!s_viewOrig) return;
    if (!g_cull || !g_cull->enabled || !span || !looksLikePointer(span->begin) ||
        span->end <= span->begin) {
        s_viewOrig(lambda, span);
        return;
    }
    const auto bytes = static_cast<std::size_t>(span->end - span->begin);
    if (bytes % 16 != 0 || bytes > kMaxSpanBytes || bytes < 16u * kMinViewEntries) {
        s_viewOrig(lambda, span);
        return;
    }

    int total = 0;
    int valid = 0;
    int culled = 0;
    int reasonCount[5] = {};
    auto* write = span->begin;
    for (auto* read = span->begin; read != span->end; read += 16) {
        void* instanced = *reinterpret_cast<void**>(read);
        ++total;
        int bx = 0;
        int by = 0;
        int bz = 0;
        if (chunkPos(instanced, bx, by, bz)) ++valid;
        int reason = 0;
        if (instanced && shouldCullChunk(instanced, true, &reason)) {
            ++culled;
            if (reason > 0 && reason < 5) ++reasonCount[reason];
            continue;
        }
        if (write != read) {
            alignas(16) std::uint8_t tmp[16];
            std::memcpy(tmp, write, 16);
            std::memcpy(write, read, 16);
            std::memcpy(read, tmp, 16);
        }
        write += 16;
    }
    auto* savedEnd = span->end;
    span->end = write;
    s_viewOrig(lambda, span);
    span->end = savedEnd;

    const int pass = g_viewPasses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_logBudget.fetch_sub(1, std::memory_order_acq_rel) > 0 || pass % 600 == 0) {
        diag("view #%d total=%d valid=%d culled=%d rRad=%d rCone=%d rBelow=%d rebuild=%d",
             pass, total, valid, culled, reasonCount[1], reasonCount[2], reasonCount[4],
             g_rebuildFrames.load(std::memory_order_relaxed));
    }
}

void onLocalTick(bedrocktools::events::LocalPlayerTickEvent& event) {
    if (!event.player) return;
    const auto pos = event.player->position();
    if (!finiteCoord(pos.x) || !finiteCoord(pos.y) || !finiteCoord(pos.z)) return;
    g_playerX = pos.x;
    g_playerY = pos.y;
    g_playerZ = pos.z;
    const auto rot = event.player->rotation();
    const float yaw = rot.y * 0.017453292f;
    g_fwdX = -std::sin(yaw);
    g_fwdZ = std::cos(yaw);
    g_havePlayer = true;
}

}

HeavyCullModule::HeavyCullModule()
    : Module("Heavy Cull",
             "Builds and draws only the terrain around you. Chunks Past Radius (default 12) is the distance still drawn; lower is faster and shows less world.") {
    g_cull = this;
    moduleId = "volt.heavycull";
    masterEnabled = true;
    updateEnabledState();
}

HeavyCullModule::~HeavyCullModule() {
    if (g_cull == this) g_cull = nullptr;
}

void HeavyCullModule::onInit() {
    static bool diagReset = false;
    if (!diagReset) {
        if (FILE* file = std::fopen(kDiagPath, "w")) std::fclose(file);
        diagReset = true;
    }
    static bool eventsWired = false;
    if (!eventsWired) {
        bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(onLocalTick);
        eventsWired = true;
    }
    if (!s_scheduleHook) {
        const auto addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ScheduleChunkBuild);
        if (addr) {
            s_scheduleHook = bedrocktools::hooks::install(reinterpret_cast<void*>(addr),
                                                          reinterpret_cast<void*>(scheduleChunkBuildDetour),
                                                          reinterpret_cast<void**>(&s_scheduleOrig));
            diag("schedule hook addr=%p ok=%d", reinterpret_cast<void*>(addr), s_scheduleHook ? 1 : 0);
        } else {
            diag("schedule signature unavailable");
        }
    }
    if (!s_viewHook) {
        const auto addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SetupViewArea);
        if (addr) {
            s_viewHook = bedrocktools::hooks::install(reinterpret_cast<void*>(addr),
                                                      reinterpret_cast<void*>(setupViewAreaDetour),
                                                      reinterpret_cast<void**>(&s_viewOrig));
            diag("view hook addr=%p ok=%d", reinterpret_cast<void*>(addr), s_viewHook ? 1 : 0);
        } else {
            diag("view signature unavailable");
        }
    }
    m_hooked = s_scheduleHook != nullptr && s_viewHook != nullptr;
    diag("init hooked=%d cull=%d below=%d cone=%d behind=%d",
         m_hooked ? 1 : 0, cullChunks, belowBlocks, viewCone, behindCamera ? 1 : 0);
}

void HeavyCullModule::onEnable() {
    if (!m_hooked) onInit();
    g_rebuildFrames.store(kRebuildFrames, std::memory_order_release);
    diag("enable rebuildFrames=%d", kRebuildFrames);
}

void HeavyCullModule::onDisable() {
    g_rebuildFrames.store(kRebuildFrames, std::memory_order_release);
    diag("disable");
}

void HeavyCullModule::onFrame() {
    const int left = g_rebuildFrames.load(std::memory_order_acquire);
    if (left > 0) g_rebuildFrames.store(left - 1, std::memory_order_release);
}

void HeavyCullModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("cullChunks")) cullChunks = j["cullChunks"].get<int>();
    if (j.contains("belowBlocks")) belowBlocks = j["belowBlocks"].get<int>();
    if (j.contains("viewCone")) viewCone = j["viewCone"].get<int>();
    if (j.contains("behindCamera")) behindCamera = j["behindCamera"].get<bool>();
    cullChunks = std::clamp(cullChunks, kMinCullChunks, kMaxCullChunks);
    belowBlocks = std::clamp(belowBlocks, 1, 64);
    viewCone = std::clamp(viewCone, 60, 180);
}

void HeavyCullModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["cullChunks"] = cullChunks;
    j["belowBlocks"] = belowBlocks;
    j["viewCone"] = viewCone;
    j["behindCamera"] = behindCamera;
}
