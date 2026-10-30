#include "GameHooks.hpp"
#include <bedrocktools/Version.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <EGL/egl.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace bedrocktools::core::gamehooks {
namespace {
using namespace bedrocktools::events;
using bedrocktools::memory::SignatureId;

using VersionStringFn = std::string(*)(void*);
using NormalTickFn = void(*)(void*);
using ClientInstanceUpdateFn = void*(*)(void*, bool);
using EglSwapBuffersFn = EGLBoolean(*)(EGLDisplay, EGLSurface);
using EglSwapIntervalFn = EGLBoolean(*)(EGLDisplay, EGLint);

VersionStringFn versionOriginal = nullptr;
NormalTickFn tickOriginal = nullptr;
ClientInstanceUpdateFn clientUpdateOriginal = nullptr;
EglSwapBuffersFn swapBuffersOriginal = nullptr;
EglSwapIntervalFn swapIntervalOriginal = nullptr;
std::atomic<void*> currentClientInstance = nullptr;
std::atomic<int> forcedSwapInterval{-1};
std::array<bedrocktools::hooks::Handle, 8> handles{};
std::size_t handleCount = 0;
std::mutex installMutex;
bool installed = false;
std::string gameVersion;

template <class Function>
bool hookSignature(SignatureId id, void* detour, Function** original) {
    const auto address = bedrocktools::memory::resolve(id);
    if (!address) return false;
    bedrocktools::hooks::Handle handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, reinterpret_cast<void**>(original));
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}

std::string versionDetour(void* self) {
    std::string version = versionOriginal ? versionOriginal(self) : std::string{};
    if (gameVersion.empty()) gameVersion = version;
    return std::string("\xC2\xA7" "b") + std::string(bedrocktools::Name) + " v" + std::string(bedrocktools::Version) + " " + "\xC2\xA7" "fby " + "\xC2\xA7" "e" + std::string(bedrocktools::Author) + " " + "\xC2\xA7" "f- " + "\xC2\xA7" "r" + version;
}

void tickDetour(void* actor) {
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    // The NormalTick signature is a generic prologue and may resolve to a function whose `this`
    // is not an initialized Actor. Only publish when `actor` actually exposes the Actor component
    // block; otherwise a handler would dereference a bogus component pointer and fault.
    // NormalTick is an Actor virtual, so it runs for every ticking actor.
    // The culling modules need the player, never a mob: the category bit is
    // the cheapest check that does not depend on a level layout offset.
    const bool isPlayer = player != nullptr && player->stateVectorComponent() != nullptr &&
                          player->isPlayer();
    if (isPlayer) {
        LocalPlayerPreTickEvent preEvent{player};
        bus().publish(preEvent);
    }
    if (tickOriginal) tickOriginal(actor);
    if (isPlayer) {
        LocalPlayerTickEvent event{player};
        bus().publish(event);
    }
}

void* clientUpdateDetour(void* clientInstance, bool value) {
    if (clientInstance) currentClientInstance.store(clientInstance, std::memory_order_release);
    void* result = clientUpdateOriginal ? clientUpdateOriginal(clientInstance, value) : nullptr;
    ClientInstanceUpdateEvent event{reinterpret_cast<bedrocktools::sdk::ClientInstance*>(clientInstance)};
    bus().publish(event);
    return result;
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        FrameEvent event;
        bus().publish(event);
    }
    return swapBuffersOriginal ? swapBuffersOriginal(display, surface) : EGL_FALSE;
}

EGLBoolean swapIntervalDetour(EGLDisplay display, EGLint interval) {
    const int forced = forcedSwapInterval.load(std::memory_order_acquire);
    if (forced >= 0) interval = forced;
    return swapIntervalOriginal ? swapIntervalOriginal(display, interval) : EGL_TRUE;
}

bool hookEgl() {
    auto egl = bedrocktools::hooks::openLibrary("libEGL.so");
    if (!egl) return false;

    const auto swapBuffers = bedrocktools::hooks::symbol(egl, "eglSwapBuffers");
    const auto swapInterval = bedrocktools::hooks::symbol(egl, "eglSwapInterval");
    bool ok = false;

    if (swapBuffers) {
        auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(swapBuffers), reinterpret_cast<void*>(swapBuffersDetour), reinterpret_cast<void**>(&swapBuffersOriginal));
        if (handle) {
            if (handleCount < handles.size()) handles[handleCount++] = handle;
            ok = true;
        }
    }
    if (swapInterval) {
        auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(swapInterval), reinterpret_cast<void*>(swapIntervalDetour), reinterpret_cast<void**>(&swapIntervalOriginal));
        if (handle) {
            if (handleCount < handles.size()) handles[handleCount++] = handle;
        }
    }

    bedrocktools::hooks::closeLibrary(egl);
    return ok;
}
}

bool install() {
    std::lock_guard lock(installMutex);
    if (installed) return true;
    hookSignature(SignatureId::VersionString, reinterpret_cast<void*>(versionDetour), &versionOriginal);
    hookSignature(SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal);
    hookSignature(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal);
    hookEgl();

    installed = swapBuffersOriginal != nullptr;
    return installed;
}

void uninstall() {
    std::lock_guard lock(installMutex);
    for (std::size_t i = 0; i < handleCount; ++i) {
        if (handles[i]) bedrocktools::hooks::remove(handles[i]);
        handles[i] = nullptr;
    }
    handleCount = 0;
    installed = false;
    forcedSwapInterval.store(-1, std::memory_order_release);
    currentClientInstance.store(nullptr, std::memory_order_release);
}

void* clientInstance() {
    return currentClientInstance.load(std::memory_order_acquire);
}

void setForcedSwapInterval(int interval) {
    forcedSwapInterval.store(interval, std::memory_order_release);
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) return;
    const int value = interval >= 0 ? interval : 1;
    if (swapIntervalOriginal) swapIntervalOriginal(display, value);
    else eglSwapInterval(display, value);
}

}
