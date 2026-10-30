# Changelog

## 2.0.0

Complete rewrite against Minecraft Bedrock **1.26.60.04_RC3**.

### Fixed

- **Invisible world.** 2.4.x loaded a 1.26.52 `RenderChunk.material.bin`
  (255,988 B) into the 1.26.60 renderer, whose real file is 256,005 B. Terrain
  meshes never rendered while entities did. The material/LOD subsystem is
  removed in 2.0; no game asset is read, patched or replaced.
- **Config wiped by the launcher.** A missing `config.json` used to be written
  but not applied, so every module started at its default while the file said
  otherwise. The loader now writes defaults and applies the same object.
- **Half-written config.** Saves are atomic (`config.json.tmp` + rename). An
  unparsable file is copied to `config.json.broken` and left in place.
- **Wrong player for culling.** `NormalTick` is an actor virtual, so the tick
  event used to carry mobs as well as the player. Tick events are now
  published only for actors with the Player category.
- **Opaque Leaves crash path.** The option-observer notify call was disabled in
  2.4.x because calling it from the menu thread crashed the game; the dead
  path is gone. The forced chunk-remesh call (which passed a bogus
  `LevelChunk` in the fourth argument) is also gone.

### Removed

- Chunk LOD, Tight Fog, No Particles, Still Textures and the whole
  `src/render/materials` tree.
- `AAsset` interception and terrain material patching.
- Chunk remesh forcing (`src/render/ChunkRemesh`).
- Heavy Cull `surfaceOnly` and its surface-height grid.
- RAM Optimizer entity despawning (`mallopt(M_PURGE)` stays; the module no
  longer deletes mobs).
- GPU Cuts code patches (`nops=0` on 1.26.60 anyway); it now writes only
  renderer options that the game itself exposes.
- FPS Unlock's `gfx_max_framerate` write; the frame cap is done with the EGL
  swap interval instead.

### Changed

- Module order: Heavy Cull is registered before Fix SpikeLag, so culled chunks
  never spend the pacing budget.
- New defaults: Heavy Cull on with a 12-chunk radius, `behindCamera=false`,
  `viewCone=180`, `belowBlocks=24`; Fix SpikeLag off by default; GPU Cuts
  options-only.
- `Heavy Cull` no longer touches renderer fields. Culling is decided from the
  chunk position and the player position, so a layout change degrades to "no
  culling" instead of "no world".
- `RAM Optimizer` config keys are now `intervalSeconds` (5-300) and
  `onlyWhenIdle`.
- Every module has a stable id without spaces (`volt.heavycull`,
  `volt.fpsunlock`, ...).
- Menu help text rewritten to match the remaining settings only.

### Compatibility

- Target build: `1.26.60.04_RC3`, arm64-v8a.
- Older or newer builds are not supported; the signature table is verified
  against this one `libminecraftpe.so`.
