#pragma once

#include "modules/Module.hpp"

class OpaqueLeavesModule : public Module {
public:
    OpaqueLeavesModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
};
