#include "FPSUnlocker.hpp"
#include "core/GameHooks.hpp"
#include <EGL/egl.h>
#include <time.h>
#include <errno.h>

FPSUnlockerModule::FPSUnlockerModule()
    : Module("FPS Unlock", "Unlock VSync. Target FPS 0 = unlimited, or cap at 60/90/120.") {
    moduleId = "volt.fpsunlock";
    masterEnabled = true;
    updateEnabledState();
}

void FPSUnlockerModule::onInit() {}

void FPSUnlockerModule::onEnable() {
    m_hasLastFrame = false;
    bedrocktools::core::gamehooks::setForcedSwapInterval(0);
}

void FPSUnlockerModule::onDisable() {
    bedrocktools::core::gamehooks::setForcedSwapInterval(-1);
    m_hasLastFrame = false;
}

void FPSUnlockerModule::onFrame() {
    EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY) {
        eglSwapInterval(display, 0);
    }

    if (targetFPS <= 0) {
        m_hasLastFrame = false;
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (m_hasLastFrame) {
        long long frameDurationNs = 1000000000LL / targetFPS;

        struct timespec deadline;
        deadline.tv_sec = m_lastFrameTime.tv_sec;
        deadline.tv_nsec = m_lastFrameTime.tv_nsec + frameDurationNs;

        while (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
        }

        m_lastFrameTime = deadline;
    } else {
        m_lastFrameTime = now;
        m_hasLastFrame = true;
    }
}

void FPSUnlockerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("targetFPS")) targetFPS = j["targetFPS"].get<int>();
    if (targetFPS < 0) targetFPS = 0;
    if (targetFPS > 120) targetFPS = 120;
}

void FPSUnlockerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["targetFPS"] = targetFPS;
}
