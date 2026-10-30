#include "FixSpikeLag.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {

using ScheduleChunkBuildFn = bool (*)(void*, void*, void*, void*, void*);
ScheduleChunkBuildFn s_orig = nullptr;
bedrocktools::hooks::Handle s_hook = nullptr;

FixSpikeLagModule* g_module = nullptr;

constexpr std::int64_t kUsPerMs = 1000;
constexpr std::int64_t kMoveHoldUs = 500 * kUsPerMs;
constexpr float kMoveEpsilon2 = 0.0004f;
constexpr float kLookEpsilonDeg = 0.15f;
constexpr float kTeleportDist2 = 64.0f * 64.0f;
constexpr std::int64_t kTeleportGraceUs = 2000 * kUsPerMs;
constexpr int kWarmupCalls = 512;   // vanilla speed until the world is up
constexpr std::int64_t kWindowBudgetUs = 6000;

std::atomic<int> s_totalCalls{0};
std::atomic<std::int64_t> s_windowStartUs{0};
std::atomic<int> s_builtInWindow{0};
std::atomic<std::int64_t> s_spentInWindowUs{0};
std::atomic<int> s_coolFramesLeft{0};
std::atomic<int> s_stopFramesLeft{0};
std::atomic<std::int64_t> s_lastFrameUs{0};
std::atomic<std::int64_t> s_lastMoveUs{0};
std::atomic<std::int64_t> s_teleportUntilUs{0};

float s_lastX = 0.0f;
float s_lastZ = 0.0f;
float s_lastYaw = 0.0f;
float s_lastPitch = 0.0f;
bool s_haveLastPos = false;
bool s_haveLastRot = false;

std::int64_t nowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

float wrapDegrees(float delta) {
    delta = std::fmod(delta + 540.0f, 360.0f);
    if (delta < 0.0f) delta += 360.0f;
    return delta - 180.0f;
}

bool scheduleChunkBuildDetour(void* self, void* instanced, void* sharedPtr, void* levelChunk, void* unused) {
    if (!g_module || !g_module->enabled || g_module->maxBuilds <= 0) {
        return s_orig ? s_orig(self, instanced, sharedPtr, levelChunk, unused) : false;
    }

    const int total = s_totalCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (total <= kWarmupCalls) return s_orig ? s_orig(self, instanced, sharedPtr, levelChunk, unused) : false;

    const std::int64_t now = nowUs();
    if (now < s_teleportUntilUs.load(std::memory_order_relaxed)) {
        return s_orig ? s_orig(self, instanced, sharedPtr, levelChunk, unused) : false;
    }

    const bool pacing = !g_module->onlyWhileMoving ||
                        (now - s_lastMoveUs.load(std::memory_order_relaxed)) < kMoveHoldUs;
    if (!pacing) return s_orig ? s_orig(self, instanced, sharedPtr, levelChunk, unused) : false;

    const auto windowUs = static_cast<std::int64_t>(g_module->windowMs) * kUsPerMs;
    if (now - s_windowStartUs.load(std::memory_order_relaxed) >= windowUs) {
        s_windowStartUs.store(now, std::memory_order_relaxed);
        s_builtInWindow.store(0, std::memory_order_relaxed);
        s_spentInWindowUs.store(0, std::memory_order_relaxed);
    }

    int cap = g_module->maxBuilds;
    if (g_module->adaptive) {
        if (s_stopFramesLeft.load(std::memory_order_relaxed) > 0) cap = 0;
        else if (s_coolFramesLeft.load(std::memory_order_relaxed) > 0) cap = 1;
    }

    const int built = s_builtInWindow.load(std::memory_order_relaxed);
    if (built > 0 && built >= cap) return false;

    const std::int64_t spent = s_spentInWindowUs.load(std::memory_order_relaxed);
    if (built > 0 && spent >= kWindowBudgetUs) return false;

    const std::int64_t callStart = nowUs();
    const bool ok = s_orig ? s_orig(self, instanced, sharedPtr, levelChunk, unused) : false;
    if (ok) {
        s_builtInWindow.fetch_add(1, std::memory_order_relaxed);
        s_spentInWindowUs.fetch_add(nowUs() - callStart, std::memory_order_relaxed);
    }
    return ok;
}

void onLocalTick(bedrocktools::events::LocalPlayerTickEvent& event) {
    if (!event.player) return;

    const auto pos = event.player->position();
    if (std::isfinite(pos.x) && std::isfinite(pos.z)) {
        if (s_haveLastPos) {
            const float dx = pos.x - s_lastX;
            const float dz = pos.z - s_lastZ;
            const float distance2 = dx * dx + dz * dz;
            if (distance2 > kTeleportDist2) {
                s_teleportUntilUs.store(nowUs() + kTeleportGraceUs, std::memory_order_relaxed);
            } else if (distance2 > kMoveEpsilon2) {
                s_lastMoveUs.store(nowUs(), std::memory_order_relaxed);
            }
        }
        s_lastX = pos.x;
        s_lastZ = pos.z;
        s_haveLastPos = true;
    }

    const auto rot = event.player->rotation();
    if (std::isfinite(rot.x) && std::isfinite(rot.y)) {
        if (s_haveLastRot) {
            const float deltaYaw = wrapDegrees(rot.y - s_lastYaw);
            const float deltaPitch = rot.x - s_lastPitch;
            if (std::fabs(deltaYaw) > kLookEpsilonDeg || std::fabs(deltaPitch) > kLookEpsilonDeg) {
                s_lastMoveUs.store(nowUs(), std::memory_order_relaxed);
            }
        }
        s_lastYaw = rot.y;
        s_lastPitch = rot.x;
        s_haveLastRot = true;
    }
}

}

FixSpikeLagModule::FixSpikeLagModule()
    : Module("Fix SpikeLag",
             "Paces chunk rebuilds while walking so new terrain does not freeze the frame. Off by default: turn it on if exploring stutters.") {
    moduleId = "volt.fixspikelag";
    g_module = this;
    masterEnabled = false;
    updateEnabledState();
}

FixSpikeLagModule::~FixSpikeLagModule() {
    if (g_module == this) g_module = nullptr;
}

void FixSpikeLagModule::onInit() {
    static bool eventsWired = false;
    if (!eventsWired) {
        bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(onLocalTick);
        eventsWired = true;
    }
    if (!s_hook) {
        const auto addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ScheduleChunkBuild);
        if (addr) {
            s_hook = bedrocktools::hooks::install(reinterpret_cast<void*>(addr),
                                                  reinterpret_cast<void*>(scheduleChunkBuildDetour),
                                                  reinterpret_cast<void**>(&s_orig),
                                                  pl::memory::HookPriority::Low);
        }
    }
    m_hooked = s_hook != nullptr;
}

void FixSpikeLagModule::onEnable() {
    if (!m_hooked) onInit();
    s_totalCalls.store(0, std::memory_order_relaxed);
    s_lastFrameUs.store(0, std::memory_order_relaxed);
    s_coolFramesLeft.store(0, std::memory_order_relaxed);
    s_stopFramesLeft.store(0, std::memory_order_relaxed);
    s_windowStartUs.store(0, std::memory_order_relaxed);
    s_builtInWindow.store(0, std::memory_order_relaxed);
    s_spentInWindowUs.store(0, std::memory_order_relaxed);
    s_teleportUntilUs.store(0, std::memory_order_relaxed);
}

void FixSpikeLagModule::onDisable() {
    s_coolFramesLeft.store(0, std::memory_order_relaxed);
    s_stopFramesLeft.store(0, std::memory_order_relaxed);
}

void FixSpikeLagModule::onFrame() {
    const std::int64_t now = nowUs();
    const std::int64_t last = s_lastFrameUs.exchange(now, std::memory_order_relaxed);
    if (last == 0 || !adaptive) return;

    const std::int64_t frameUs = now - last;
    const auto spikeUs = static_cast<std::int64_t>(spikeMs) * kUsPerMs;
    if (frameUs > spikeUs) {
        s_coolFramesLeft.store(cooldownFrames, std::memory_order_relaxed);
        if (frameUs > spikeUs * 2) s_stopFramesLeft.store(std::min(cooldownFrames, 12), std::memory_order_relaxed);
    } else {
        const int cool = s_coolFramesLeft.load(std::memory_order_relaxed);
        if (cool > 0) s_coolFramesLeft.store(cool - 1, std::memory_order_relaxed);
        const int stop = s_stopFramesLeft.load(std::memory_order_relaxed);
        if (stop > 0) s_stopFramesLeft.store(stop - 1, std::memory_order_relaxed);
    }
}

void FixSpikeLagModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("maxBuilds")) maxBuilds = j["maxBuilds"].get<int>();
    if (j.contains("windowMs")) windowMs = j["windowMs"].get<int>();
    if (j.contains("onlyWhileMoving")) onlyWhileMoving = j["onlyWhileMoving"].get<bool>();
    if (j.contains("adaptive")) adaptive = j["adaptive"].get<bool>();
    if (j.contains("spikeMs")) spikeMs = j["spikeMs"].get<int>();
    if (j.contains("cooldownFrames")) cooldownFrames = j["cooldownFrames"].get<int>();
    maxBuilds = std::clamp(maxBuilds, 0, 16);
    windowMs = std::clamp(windowMs, 4, 64);
    spikeMs = std::clamp(spikeMs, 16, 250);
    cooldownFrames = std::clamp(cooldownFrames, 0, 200);
}

void FixSpikeLagModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["maxBuilds"] = maxBuilds;
    j["windowMs"] = windowMs;
    j["onlyWhileMoving"] = onlyWhileMoving;
    j["adaptive"] = adaptive;
    j["spikeMs"] = spikeMs;
    j["cooldownFrames"] = cooldownFrames;
}
