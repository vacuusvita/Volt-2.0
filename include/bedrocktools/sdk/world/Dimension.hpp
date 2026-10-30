#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>
#include <bedrocktools/sdk/world/Weather.hpp>

namespace bedrocktools::sdk {
class Dimension {
public:
    Weather* weather() { return field<Weather*>(this, offsets::Dimension::mWeather); }
};
}
