#include <bedrocktools/memory/Signatures.hpp>

#include <array>
#include <string>
#include <vector>
#include <pl/memory/Signature.hpp>

namespace bedrocktools::memory {
namespace {
std::array<std::uintptr_t, SignatureCount> addresses{};
const std::array<SignatureDefinition, SignatureCount> definitions{{
    // Buffered version-label writer. Optional: the mod still loads when the
    // label hook is skipped.
    SignatureDefinition{SignatureId::VersionString,
                        "? ? ? A9 ? ? ? F9 FD 03 00 91 F3 03 08 AA ? ? ? ? E1 03 00 AA ? ? ? ? ? ? ? 91"},
    SignatureDefinition{SignatureId::NormalTick,
                        "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39"},
    // Carries the 0xdeadc0de frame magic; detour keeps the 2-arg ABI.
    SignatureDefinition{SignatureId::ClientInstanceUpdate,
                        "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 59 D0 3B D5 ? ? ? 52 F5 03 02 2A"},
    SignatureDefinition{SignatureId::DimensionTick,
                        "? ? ? D1 ? ? ? FD ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5C D0 3B D5 1F 20 03 D5"},
    SignatureDefinition{SignatureId::WeatherTick,
                        "? ? ? A9 ? ? ? A9 FD 03 00 91 F3 03 00 AA ? ? ? F9 ? ? ? 39 ? ? ? 34"},
    SignatureDefinition{SignatureId::WeatherGetRainLevel,
                        "? ? ? F9 ? ? ? 39 ? ? ? 34 ? ? ? 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E C0 03 5F D6 00 E4 00 2F C0 03 5F D6 ? ? ? BD"},
    SignatureDefinition{SignatureId::WeatherGetLightningLevel,
                        "? ? ? F9 ? ? ? 39 ? ? ? 34 ? ? ? 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E C0 03 5F D6 00 E4 00 2F C0 03 5F D6 ? ? ? F9"},
    SignatureDefinition{SignatureId::WeatherIsRaining,
                        "? ? ? 52 ? ? ? F9 00 01 22 1E ? ? ? 39 ? ? ? 34 ? ? ? 2D 42 38 21 1E 40 08 20 1E 20 28 20 1E ? ? ? 52 ? ? ? 72 01 01 27 1E 10 20 21 1E E0 D7 9F 1A"},
    SignatureDefinition{SignatureId::WeatherIsLightning,
                        "? ? ? F9 ? ? ? 39 ? ? ? 34 ? ? ? 2D ? ? ? 52 ? ? ? 2D 04 01 22 1E"},
    // LevelBuilder::scheduleChunkBuild(RenderChunkInstanced&,
    //   const std::shared_ptr<RenderChunkShared>&, LevelChunk&)
    SignatureDefinition{SignatureId::ScheduleChunkBuild,
                        "E9 23 B9 6D FD 7B 01 A9 FC 6F 02 A9 FA 67 03 A9 F8 5F 04 A9 F6 57 05 A9 F4 4F 06 A9 FD 43 00 91 FF C3 0E D1"},
    // Reached through a vtable/callback (no direct caller): matched on the
    // prologue plus the stack-guard mrs. 1.26.60.04_RC3: 0xb7f8d34.
    SignatureDefinition{SignatureId::SetupViewArea,
                        "? ? ? ? FD 7B 05 A9 FC 6F 06 A9 FA 67 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 ? ? ? ? 48 D0 3B D5 E8 07 00 F9 08 15 40 F9 A8 83 1F F8 19 04 40 F9 3A 6C 40 A9"},
    SignatureDefinition{SignatureId::QueueRenderActorFilter,
                        "FF 83 02 D1 E8 2B 00 FD FD 7B 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F 09 A9 FD 83 01 91 57 D0 3B D5 F3 03 00 AA F4 03 01 AA E8 16 40 F9 A8 83 1E F8 08 04 40 F9 15 10 40 F9 08 31 41 39 C8 00 00 34"},
    SignatureDefinition{SignatureId::RenderBlockActorFilter,
                        "FF 03 02 D1 FD 7B 03 A9 FA 67 04 A9 F8 5F 05 A9 F6 57 06 A9 F4 4F 07 A9 FD C3 00 91 58 D0 3B D5 08 17 40 F9 A8 83 1F F8 01 18 00 B4 28 00 40 F9 F3 03 00 AA E0 03 01 AA F5 03 02 AA F4 03 01 AA 08 09 40 F9 00 01 3F D6"},
    SignatureDefinition{SignatureId::BlockSourceIsSolidBlockingBlock,
                        "? ? ? F9 ? ? ? B9 ? ? ? 29 ? ? ? F9 E1 03 08 2A"},
    // BaseOptionRegistry::getOption(OptionID). The option array starts at
    // +0x20 and stores one Option* per id. 1.26.60.04_RC3: 0xb090ef0
    // (`cmp w1, #0x338` then `add x9, x0, w1, uxtw #3; ldr x0, [x9, #0x20]`).
    SignatureDefinition{SignatureId::OptionRegistryGetOption,
                        "? ? ? ? FD 7B 04 A9 F4 4F 05 A9 ? ? ? ? 48 D0 3B D5 09 15 40 F9 ? ? ? ? A9 83 1F F8 ? ? ? ? 09 4C 21 8B 20 11 40 F9 ? ? ? ?"},
}};
}

bool resolveAll(std::string_view libraryName) {
    const std::string library(libraryName);
    std::vector<std::string> patterns;
    patterns.reserve(definitions.size());
    for (const auto& definition : definitions) patterns.emplace_back(definition.pattern);
    const auto resolved = pl::memory::resolveSignatures(patterns, library.c_str());
    addresses.fill(0);
    bool any = false;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto it = resolved.find(patterns[i]);
        if (it == resolved.end() || it->second == 0) continue;
        addresses[static_cast<std::size_t>(definitions[i].id)] = it->second;
        any = true;
    }
    return any;
}

std::uintptr_t resolve(SignatureId id) {
    const auto index = static_cast<std::size_t>(id);
    return index < addresses.size() ? addresses[index] : 0;
}

void clear() {
    addresses.fill(0);
}

}
