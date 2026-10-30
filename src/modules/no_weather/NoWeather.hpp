#pragma once

#include "modules/Module.hpp"

class NoWeatherModule : public Module {
public:
    NoWeatherModule();
    ~NoWeatherModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void installHooks();
    void applyWeather(void* weather) const;

private:
    bool m_hooked = false;
};
