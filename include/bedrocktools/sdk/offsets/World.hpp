#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace Actor {
// 1.26.60: the Actor component-pointer block moved +0x20 (a member was inserted before it).
// Verified against libminecraftpe 1.26.60:
//   Actor::getPos()     0xfd8e62c = ldr x8,[x0,#0x228]; ldp s0,s1,[x8]; ldr s2,[x8,#8]; ret
//   Actor::getRot()     0xfd869b0 = ldr x0,[x0,#0x238]; ret   (mRot at +0, yaw +0 / pitch +4)
//   Actor AABB reader   0xfd8e63c = ldr x8,[x0,#0x230]; ...
inline constexpr std::size_t mStateVectorComponent = 0x228;    // was 0x208
inline constexpr std::size_t mActorRotationComponent = 0x238;  // was 0x218
inline constexpr std::size_t mCategories = 0x220;              // was 0x200
}

namespace Dimension {
inline constexpr std::size_t mWeather = 0x1B8;
}

namespace Weather {
inline constexpr std::size_t mOldRainLevel = 0x34;
inline constexpr std::size_t mRainLevel = 0x38;
inline constexpr std::size_t mTargetRainLevel = 0x3C;
inline constexpr std::size_t mOldLightningLevel = 0x40;
inline constexpr std::size_t mLightningLevel = 0x44;
inline constexpr std::size_t mTargetLightningLevel = 0x48;
}

}
