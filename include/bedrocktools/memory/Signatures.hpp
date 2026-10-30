#pragma once

#include <bedrocktools/Export.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bedrocktools::memory {

// Every id here is resolved against libminecraftpe 1.26.60.04_RC3 and is used
// by at least one module. Ids that had no verified 1.26.60 address were removed
// instead of being left to fail at runtime.
enum class SignatureId : std::uint16_t {
    VersionString,
    NormalTick,
    ClientInstanceUpdate,
    DimensionTick,
    WeatherTick,
    WeatherGetRainLevel,
    WeatherGetLightningLevel,
    WeatherIsRaining,
    WeatherIsLightning,
    ScheduleChunkBuild,
    SetupViewArea,
    QueueRenderActorFilter,
    RenderBlockActorFilter,
    BlockSourceIsSolidBlockingBlock,
    OptionRegistryGetOption,
    Count
};

inline constexpr std::size_t SignatureCount = static_cast<std::size_t>(SignatureId::Count);

struct SignatureDefinition {
    SignatureId id;
    std::string_view pattern;
};

BEDROCKTOOLS_API bool resolveAll(std::string_view libraryName = "libminecraftpe.so");
BEDROCKTOOLS_API std::uintptr_t resolve(SignatureId id);
BEDROCKTOOLS_API void clear();

}
