#include "RamOptimizer.hpp"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstdint>

#if defined(__ANDROID__)
#include <malloc.h>

// bionic only declares mallopt() when compiling against API 26+, but the symbol
// exists on every Android the game supports. Reference it weakly so the mod
// still loads if a device does not export it.
extern "C" int voltMallopt(int option, int value) __asm__("mallopt") __attribute__((weak));

#ifndef M_PURGE
#define M_PURGE (-101)
#endif
#endif

namespace {

constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/ram_diag.log";

constexpr long long kSpikeFrameMs = 40;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void diag(const char* fmt, ...) {
    char line[256];
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

}

RamOptimizerModule::RamOptimizerModule()
    : Module("Ram Optimizer",
             "Returns free memory to the system every few seconds. Runs only while the frame time is calm, so it never adds a hitch.") {
    moduleId = "volt.ramoptimizer";
    masterEnabled = true;
    updateEnabledState();
}

void RamOptimizerModule::onEnable() {
    m_lastPurgeMs = nowMs();
    m_lastFrameMs = 0;
    m_calmFrames = 0;
}

void RamOptimizerModule::onFrame() {
    const long long now = nowMs();
    const long long previous = m_lastFrameMs;
    m_lastFrameMs = now;
    if (previous != 0 && now - previous <= kSpikeFrameMs) {
        if (m_calmFrames < 1000) ++m_calmFrames;
    } else {
        m_calmFrames = 0;
    }

    const long long interval = static_cast<long long>(std::max(intervalSeconds, 5)) * 1000;
    if (now - m_lastPurgeMs < interval) return;
    if (onlyWhenIdle && m_calmFrames < 30) return;

    m_lastPurgeMs = now;
#if defined(__ANDROID__)
    int purged = 0;
    if (voltMallopt) purged = voltMallopt(M_PURGE, 0);
    diag("purge t=%lld result=%d", now, purged);
#endif
}

void RamOptimizerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("intervalSeconds")) intervalSeconds = j["intervalSeconds"].get<int>();
    if (j.contains("onlyWhenIdle")) onlyWhenIdle = j["onlyWhenIdle"].get<bool>();
    intervalSeconds = std::clamp(intervalSeconds, 5, 300);
}

void RamOptimizerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["intervalSeconds"] = intervalSeconds;
    j["onlyWhenIdle"] = onlyWhenIdle;
}
