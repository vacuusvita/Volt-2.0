# Volt 2.0

Native FPS optimizer for **Minecraft Bedrock 1.26.60.04_RC3** on Android, loaded
through LeviLauncher's preloader.

Volt 2.0 is a from-scratch rewrite. It targets one game build only: every
signature in `include/bedrocktools/memory/Signatures.hpp` is resolved against
the 1.26.60 `libminecraftpe.so` shipped with that APK, and nothing is carried
over from older builds.

## Why 2.0 exists

The 2.4.x line injected a `RenderChunk.material.bin` taken from 1.26.52 into
the 1.26.60 renderer (255,988 B against the real 256,005 B file) so it could
patch the terrain shader for Chunk LOD / Tight Fog. The 1.26.60 shader no
longer contains the patterns those patches targeted, so the file was only
corrupted: terrain meshes were built against a broken material and the world
rendered empty while entities kept drawing.

2.0 removes that whole subsystem. There is no `AAsset` hook, no
`TerrainMaterial`, no baked material, no Chunk LOD, no Tight Fog, no
No Particles and no Still Textures. Nothing in Volt writes to the renderer or
to `assets/` any more. That is the fix.

## Modules

| Module | Default | What it does | Game surface |
| --- | --- | --- | --- |
| FPS Unlock | on | Forces `eglSwapInterval(0)` and optionally sleeps the frame to a target FPS | `eglSwapBuffers`, `eglSwapInterval` |
| GPU Cuts | on | Writes renderer keys in `options.txt` (clouds, smooth lighting, bobbing, fancy skies, upscaling, vsync, particles, render distance) | `options.txt` only |
| Opaque Leaves | on | Draws leaves in the opaque pass while keeping the Fancy texture | `Leaf*Block::getRenderLayer` vtable slots + `gfx_transparentleaves` |
| Entity Cull | on | Stops drawing actors / block entities past a distance, deep below the camera, or fully occluded | `queueRenderEntities` filter, block-actor filter |
| Heavy Cull | on | Builds and draws only the chunks within a radius, optionally only in front of the camera | `LevelBuilder::scheduleChunkBuild`, `LevelRendererCamera::setupViewArea` |
| Fix SpikeLag | off | Spreads chunk mesh builds over frames while moving | `LevelBuilder::scheduleChunkBuild` |
| No Weather | on | Forces clear skies | `Dimension::tick`, `Weather::*` getters |
| RAM Optimizer | on | Returns allocator pages to Android while the frame time is calm | `mallopt(M_PURGE)` |

### Defaults

- **Heavy Cull** — `cullChunks=12`, `belowBlocks=24`, `viewCone=180` (off),
  `behindCamera=false`. The chunk the player stands in is never culled.
- **Entity Cull** — `actorRange=32`, `blockActorRange=24`, `nearBlocks=4`,
  `belowBlocks=8`, occlusion on, other players exempt.
- **Fix SpikeLag** — `maxBuilds=8`, `windowMs=16`, pacing only while moving,
  adaptive cool-down. Disabled by default so world load is vanilla speed.
- **GPU Cuts** — `renderDistance=0` keeps the game's own distance. A value
  above 0 is written as `chunks * 16` blocks and clamped to 4-32 chunks.
- **RAM Optimizer** — one purge every 20 s, only after 30 calm frames.

## Install

1. Download `Volt-levipack` from a green GitHub Actions run.
2. Let LeviLauncher import it, or unzip it into
   `/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/mods/Volt/`.
3. Launch the game. The mod menu shows one entry per module.

The package contains `manifest.json`, `libVolt.so`, `icon.png` and
`resources/minecraft.ttf`. It does not ship materials, shaders or any game
asset.

## Config

`config/config.json` next to the installed mod:

```json
{
  "Modules": {
    "Heavy Cull": {
      "keybind": 0,
      "keybindActive": true,
      "masterEnabled": true,
      "cullChunks": 12,
      "belowBlocks": 24,
      "viewCone": 180,
      "behindCamera": false
    }
  }
}
```

Rules the loader enforces:

- A missing file is created with the current defaults **and those defaults are
  applied to the modules in the same pass** — the game never starts with an
  empty module list because the file was absent.
- An existing file is never truncated or replaced by defaults.
- An unparsable file is copied to `config.json.broken` and left untouched.
- Every save goes to `config.json.tmp` and is renamed into place, so a crash
  mid-write cannot leave half a config behind.

GPU Cuts backs up the original `options.txt` values to
`config/gfx_options_backup.json`. Disabling the module restores a key only if
the file still holds the value Volt wrote; if the player changed it in the
video settings, that value wins.

## Diagnostics

The modules append to small logs in the mod folder when something cannot be
resolved: `config_diag.log`, `heavy_diag.log`, `entitycull_diag.log`,
`gpucuts_diag.log`, `opaque_diag.log`, `ram_diag.log`. They are append-only and
not written on the steady-state path.

## Build

CI builds with the Android NDK r28c and xmake:

```bash
xmake f -y -p android -a arm64-v8a -m release --ndk=$NDK
xmake -y
```

The result is `build/android/arm64-v8a/release/Volt.levipack`. Local
syntax-check without an NDK:

```bash
clang++ -std=c++20 -fsyntax-only -I include -I src <file.cpp>
```

## Not in 2.0

Chunk LOD, Tight Fog, No Particles, Still Textures, the terrain material
patcher, chunk remesh forcing and Heavy Cull's `surfaceOnly` are gone. The
first four depended on renderer addresses or shader patterns that do not exist
in 1.26.60; the last was removed because it could hide caves the player was
standing next to.

## License

See `LICENSE`. Third-party notices in `NOTICE`.
