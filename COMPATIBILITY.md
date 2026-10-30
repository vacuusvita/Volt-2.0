# Compatibility

## Target

| Item | Value |
| --- | --- |
| Minecraft Bedrock | `1.26.60.04_RC3` |
| ABI | arm64-v8a |
| `libminecraftpe.so` | 342,231,392 B, md5 `dcd14ec39652cf542eb4e5c82ea3960f` |
| Launcher | LeviLauncher with preloader native mods |
| Volt package | `libVolt.so` + `manifest.json` |

Only this build is supported. The signature table is resolved against this
exact library; a different build fails resolution gracefully (a module whose
signature is missing stays inert) but is not a supported target.

## Signature table

`include/bedrocktools/memory/Signatures.hpp` contains 15 ids. Each one is used
by at least one module; ids without a verified 1.26.60 address were deleted
instead of left to fail at runtime.

| Id | Used by | Notes |
| --- | --- | --- |
| `VersionString` | version label | optional; the label hook is skipped if absent |
| `NormalTick` | tick events | actor virtual; events are filtered to the Player category |
| `ClientInstanceUpdate` | frame/client pointer | carries the `0xdeadc0de` frame magic |
| `DimensionTick` | No Weather | |
| `WeatherTick` | No Weather | |
| `WeatherGetRainLevel` | No Weather | |
| `WeatherGetLightningLevel` | No Weather | |
| `WeatherIsRaining` | No Weather | |
| `WeatherIsLightning` | No Weather | |
| `ScheduleChunkBuild` | Heavy Cull, Fix SpikeLag | `LevelBuilder::scheduleChunkBuild` |
| `SetupViewArea` | Heavy Cull | `LevelRendererCamera::setupViewArea`, vtable-reached |
| `QueueRenderActorFilter` | Entity Cull | per-actor render filter |
| `RenderBlockActorFilter` | Entity Cull | block-actor render filter |
| `BlockSourceIsSolidBlockingBlock` | Entity Cull | occlusion ray test |
| `OptionRegistryGetOption` | Opaque Leaves | `BaseOptionRegistry::getOption` |

Removed from 2.4.x and not present here: `SetupShadowViewArea`,
`RenderMaterialSetUniform`, `UniformNameIdInit`, `ActorInstantDespawn`,
`ActorFetchNearbyActorsSorted`, `ItemInHandRendererRenderFirstPerson`,
`HudCursor`, `ActorShaderManager*`, `OptionNotifyChanged`.

## Structure offsets

`include/bedrocktools/sdk/offsets/World.hpp` matches the 1.26.60 layout:

| Field | Offset |
| --- | --- |
| `Actor::mStateVectorComponent` | `0x228` |
| `Actor::mActorRotationComponent` | `0x238` |
| `Actor::mCategories` | `0x220` |
| `ActorCategory::Player` | bit `0` |
| `Dimension::mWeather` | `0x1B8` |
| `Weather::mRainLevel` | `0x38` |
| `Weather::mLightningLevel` | `0x44` |

Every component read goes through `plausiblePointer()` (8-byte aligned, above
`0x10000`), so a wrong offset degrades to `Vec3{0}` instead of a fault.

## Renderer assets

2.0 reads none. The 2.4.x regression came from injecting a 1.26.52
`RenderChunk.material.bin` (255,988 B) into 1.26.60, whose file is 256,005 B
(md5 `1609e4bbff96bbf30a93677b5ca072f7`). The 1.26.60 shader no longer contains
the `bakeTerrain` patterns the patcher searched for, so the patch could only
corrupt the material.

There is no `AAsset` hook, no `TerrainMaterial`, no baked LOD material and no
write to `assets/` anywhere in the source tree.

## Option keys

GPU Cuts and Opaque Leaves write only keys the 1.26.60 image itself exposes:

`gfx_toggleclouds`, `gfx_smoothlighting`, `gfx_viewbobbing`,
`gfx_damagebobbing`, `gfx_fancyskies`, `gfx_upscaling`, `gfx_vsync`,
`gfx_particleviewdistance`, `gfx_bubble_particles`, `gfx_viewdistance`,
`gfx_transparentleaves`.

`gfx_viewdistance` is in blocks (`192` = 12 chunks). Volt writes
`chunks * 16`, clamped to 4-32 chunks.

The original values are kept in `config/gfx_options_backup.json`. On disable a
key is restored only when the file still holds the value Volt wrote; a value
changed in the video settings screen wins.

## Hook chain

| Priority | Hook | Owner |
| --- | --- | --- |
| Normal | `scheduleChunkBuild` | Fix SpikeLag |
| Normal | `scheduleChunkBuild` | Heavy Cull |
| Normal | `setupViewArea` | Heavy Cull |

Heavy Cull is registered before Fix SpikeLag, so its detour is earlier in the
chain and a culled chunk never spends pacing budget. Both detours forward the
original five registers unchanged; nothing is rewritten in place.

Every detour is a pass-through while its module's `enabled` flag is false.
`enabled` is derived from `masterEnabled && keybindActive` only, so a config
load cannot leave a module half-active.

## Known limits

- **Multiplayer player tracking.** Tick events are filtered to the Player
  category. On a server with other players, a remote player's tick can update
  the cached camera position between local ticks. Single-player and Realms
  play (the common case) are unaffected. The fix is a verified local-player
  accessor, which 1.26.60 does not have in this source tree.
- **Opaque Leaves vtable probe.** Slots `123, 122, 124, 121, 125` are probed
  and the first one that passes the instruction-shape checks is hooked. A build
  whose layout the checks do not recognise leaves the module inert.
- **GPU Cuts is options-only.** It does not patch render passes. On 1.26.60 no
  render-pass build map was ever verified, so 2.4.x shipped `nops=0`; 2.0
  drops the pretence.
- **Window size.** Heavy Cull accepts a view span only when it is a multiple of
  16 bytes, at least 8 entries and at most 1 MiB; anything else is forwarded
  untouched.

## Failure behaviour

- Missing signature -> module inert, one diagnostic line, game unaffected.
- Missing config -> defaults written and applied.
- Unparsable config -> copied to `config.json.broken`, original left untouched.
- Crash during save -> `.tmp` file is ignored, previous config intact.
- Material or renderer mismatch -> impossible in 2.0; no renderer asset is
  loaded.
