#include "NoWeather.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>
#include <bedrocktools/sdk/world/Weather.hpp>
#include "core/memory/Hooks.hpp"


static NoWeatherModule* g_noWeather = nullptr;

using TickFn = void(*)(void*);
using WeatherLevelFn = float(*)(void*, float);
using WeatherStateFn = bool(*)(void*);

static TickFn s_origDimensionTick = nullptr;
static TickFn s_origWeatherTick = nullptr;
static WeatherLevelFn s_origGetRainLevel = nullptr;
static WeatherLevelFn s_origGetLightningLevel = nullptr;
static WeatherStateFn s_origIsRaining = nullptr;
static WeatherStateFn s_origIsLightning = nullptr;

static bedrocktools::hooks::Handle s_dimensionTickHookHandle = nullptr;
static bedrocktools::hooks::Handle s_weatherTickHookHandle = nullptr;
static bedrocktools::hooks::Handle s_getRainLevelHookHandle = nullptr;
static bedrocktools::hooks::Handle s_getLightningLevelHookHandle = nullptr;
static bedrocktools::hooks::Handle s_isRainingHookHandle = nullptr;
static bedrocktools::hooks::Handle s_isLightningHookHandle = nullptr;

static void forceWeather(bedrocktools::sdk::Weather* weather) {
    if (!weather) return;
    if (g_noWeather && g_noWeather->enabled) g_noWeather->applyWeather(weather);
}

static void s_dimensionTickHook(void* dimension) {
    auto* typedDimension = static_cast<bedrocktools::sdk::Dimension*>(dimension);
    auto* weather = typedDimension ? typedDimension->weather() : nullptr;
    forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
    if (s_origDimensionTick) s_origDimensionTick(dimension);
    forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
}

static void s_weatherTickHook(void* weather) {
    forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
    if (s_origWeatherTick) s_origWeatherTick(weather);
    forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
}

static float s_getRainLevelHook(void* weather, float delta) {
    if (g_noWeather && g_noWeather->enabled) {
        forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
        return 0.0f;
    }
    return s_origGetRainLevel ? s_origGetRainLevel(weather, delta) : 0.0f;
}

static float s_getLightningLevelHook(void* weather, float delta) {
    if (g_noWeather && g_noWeather->enabled) {
        forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
        return 0.0f;
    }
    return s_origGetLightningLevel ? s_origGetLightningLevel(weather, delta) : 0.0f;
}

static bool s_isRainingHook(void* weather) {
    if (g_noWeather && g_noWeather->enabled) {
        forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
        return false;
    }
    return s_origIsRaining ? s_origIsRaining(weather) : false;
}

static bool s_isLightningHook(void* weather) {
    if (g_noWeather && g_noWeather->enabled) {
        forceWeather(static_cast<bedrocktools::sdk::Weather*>(weather));
        return false;
    }
    return s_origIsLightning ? s_origIsLightning(weather) : false;
}

static bedrocktools::hooks::Handle installHook(bedrocktools::memory::SignatureId id, void* detour, void** original, bedrocktools::hooks::Handle& handle) {
    if (handle) return handle;
    auto address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;
    handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, original);
    return handle;
}

NoWeatherModule::NoWeatherModule()
    : Module("No Weather", "Clear skies. No rain, snow or lightning on the client.") {
    g_noWeather = this;
    moduleId = "volt.noweather";
    masterEnabled = true;
    updateEnabledState();
}

NoWeatherModule::~NoWeatherModule() {
    if (g_noWeather == this) g_noWeather = nullptr;
}

void NoWeatherModule::installHooks() {
    installHook(bedrocktools::memory::SignatureId::DimensionTick, reinterpret_cast<void*>(s_dimensionTickHook), reinterpret_cast<void**>(&s_origDimensionTick), s_dimensionTickHookHandle);
    installHook(bedrocktools::memory::SignatureId::WeatherTick, reinterpret_cast<void*>(s_weatherTickHook), reinterpret_cast<void**>(&s_origWeatherTick), s_weatherTickHookHandle);
    installHook(bedrocktools::memory::SignatureId::WeatherGetRainLevel, reinterpret_cast<void*>(s_getRainLevelHook), reinterpret_cast<void**>(&s_origGetRainLevel), s_getRainLevelHookHandle);
    installHook(bedrocktools::memory::SignatureId::WeatherGetLightningLevel, reinterpret_cast<void*>(s_getLightningLevelHook), reinterpret_cast<void**>(&s_origGetLightningLevel), s_getLightningLevelHookHandle);
    installHook(bedrocktools::memory::SignatureId::WeatherIsRaining, reinterpret_cast<void*>(s_isRainingHook), reinterpret_cast<void**>(&s_origIsRaining), s_isRainingHookHandle);
    installHook(bedrocktools::memory::SignatureId::WeatherIsLightning, reinterpret_cast<void*>(s_isLightningHook), reinterpret_cast<void**>(&s_origIsLightning), s_isLightningHookHandle);
    m_hooked = s_dimensionTickHookHandle || s_weatherTickHookHandle || s_getRainLevelHookHandle || s_getLightningLevelHookHandle;
}

void NoWeatherModule::onInit() {
    installHooks();
}

void NoWeatherModule::onEnable() {
    if (!m_hooked) installHooks();
}

void NoWeatherModule::onDisable() {}

void NoWeatherModule::applyWeather(void* weather) const {
    auto* state = static_cast<bedrocktools::sdk::Weather*>(weather);
    if (!state) return;
    state->setRainLevel(0.0f);
    state->setLightningLevel(0.0f);
}
