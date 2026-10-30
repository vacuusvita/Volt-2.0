#include "ModuleRegistry.hpp"

#include "entity_cull/EntityCull.hpp"
#include "fix_spikelag/FixSpikeLag.hpp"
#include "fps_unlock/FPSUnlocker.hpp"
#include "gpu_cuts/GpuCuts.hpp"
#include "heavy_cull/HeavyCull.hpp"
#include "no_weather/NoWeather.hpp"
#include "opaque_leaves/OpaqueLeaves.hpp"
#include "ram_optimizer/RamOptimizer.hpp"

ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry registry;
    return registry;
}

Module* ModuleRegistry::find(std::string_view id) const {
    const auto it = mById.find(id);
    return it == mById.end() ? nullptr : it->second;
}

const std::vector<Module*>& ModuleRegistry::modules() const {
    return mView;
}

void ModuleRegistry::initialize() {
    if (mInitialized) return;
    for (auto* module : mView) module->initialize();
    mInitialized = true;
}

void ModuleRegistry::suspendAll() {
    for (auto* module : mView) module->suspendRuntime();
}

void ModuleRegistry::resumeAll() {
    for (auto* module : mView) module->resumeRuntime();
}

void ModuleRegistry::onFrame() {
    for (auto* module : mView) {
        if (module->enabled) module->onFrame();
    }
}

bool ModuleRegistry::onMouseEvent(int button, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) {
        if (module->enabled && module->onMouseEvent(button, isDown)) consumed = true;
    }
    return consumed;
}

void ModuleRegistry::setKeybindBlocked(bool blocked) {
    mKeybindBlocked = blocked;
}

bool ModuleRegistry::keybindBlocked() const {
    return mKeybindBlocked;
}

void registerAllModules() {
    auto& registry = ModuleRegistry::get();
    if (!registry.modules().empty()) return;

    // Chunk pipeline order matters: HeavyCull is registered before FixSpikeLag
    // so its detour runs first and culled chunks never consume pacing budget.
    registry.emplace<FPSUnlockerModule>();
    registry.emplace<GpuCutsModule>();
    registry.emplace<OpaqueLeavesModule>();
    registry.emplace<EntityCullModule>();
    registry.emplace<HeavyCullModule>();
    registry.emplace<FixSpikeLagModule>();
    registry.emplace<NoWeatherModule>();
    registry.emplace<RamOptimizerModule>();
}
