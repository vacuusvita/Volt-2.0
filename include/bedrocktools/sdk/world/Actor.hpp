#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>

namespace bedrocktools::sdk {

// A live component pointer is an 8-byte-aligned heap address. Android/ScaLOP keeps its pointer
// tag in the top byte, so only size + alignment are checked here. A stale/incorrect component
// offset reads a small integer (the 1.26.60 crash read 0x2d) which this rejects, so a wrong
// offset degrades to Vec3{0} instead of faulting on the load.
inline bool plausiblePointer(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v > 0x10000 && (v & 7) == 0;
}

class Actor {
public:
    void* stateVectorComponent() const {
        void* component = field<void*>(this, offsets::Actor::mStateVectorComponent);
        return plausiblePointer(component) ? component : nullptr;
    }
    void* rotationComponent() const {
        void* component = field<void*>(this, offsets::Actor::mActorRotationComponent);
        return plausiblePointer(component) ? component : nullptr;
    }
    std::uint32_t categories() const { return field<std::uint32_t>(this, offsets::Actor::mCategories); }
    Vec3 position() const {
        auto* component = stateVectorComponent();
        return component ? field<Vec3>(component, 0) : Vec3{};
    }
    Vec2 rotation() const {
        auto* component = rotationComponent();
        return component ? field<Vec2>(component, 0) : Vec2{};
    }
    bool hasCategory(std::uint32_t category) const { return (categories() & category) != 0; }
    // ActorCategory bit 0 is Player on 1.26.60 (same bit EntityCull uses).
    bool isPlayer() const { return hasCategory(1); }
};
class Player : public Actor {};
}
