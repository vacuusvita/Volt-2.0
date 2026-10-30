#include "OpaqueLeaves.hpp"
#include "config/OptionsFile.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <pl/memory/Vtable.hpp>

#include <android/log.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using bedrocktools::config::parseOptions;
using bedrocktools::config::serializeOptions;

constexpr const char* kOptionKey = "gfx_transparentleaves";
constexpr std::uint32_t kTransparentLeavesOptionId = 40;

// Block::getRenderLayer() is a virtual override on the leaf block classes.
// RenderlayerOpaque (5) is what vanilla uses for Fast leaves: it drives the
// opaque terrain pass, so the leaf texture stops being cut out in Fancy mode.
// The slot index moved between game builds (122 on 1.26.52, 123 on 1.26.60),
// so the hooks probe a short candidate list and keep the first slot that
// passes the instruction-shape checks below.  A build the checks do not
// recognise leaves the module inert instead of hooking the wrong function.
constexpr std::size_t kRenderLayerSlots[] = {123, 122, 124, 121, 125};
constexpr std::size_t kGraphicsModeSlots[] = {101, 100, 102, 99, 103};
constexpr std::uint8_t kOpaqueRenderLayer = 5;    // RenderlayerOpaque
constexpr std::size_t kLayerFieldOffset = 0x132;
constexpr std::size_t kFancyFieldOffset = 0x3a8;

// BaseOptionRegistry keeps its Option* table at +0x20.  Option stores the
// current boolean at +0x10 and its shared OptionData at +0x8.
constexpr std::size_t kRegistryOptionsOffset = 0x20;
constexpr std::size_t kOptionDataOffset = 0x8;
constexpr std::size_t kOptionValueOffset = 0x10;

constexpr const char* kDiagPath =
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/"
    "mods/Volt/opaque_diag.log";

using GetOptionFn = void* (*)(void*, std::uint32_t);

GetOptionFn s_getOptionOriginal = nullptr;
bedrocktools::hooks::Handle s_getOptionHook = nullptr;

std::atomic<void*> s_registry{nullptr};
std::atomic<void*> s_option{nullptr};
std::atomic<bool> s_forceLive{false};
std::atomic<bool> s_forceOpaque{false};
std::atomic<bool> s_savedValueValid{false};
std::atomic<int> s_savedValue{1};

#define VOLT_OPAQUE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "VoltOpaque", __VA_ARGS__)

std::atomic<int> s_layerCalls{0};

// The launcher log is not always reachable from Termux, so keep a tiny
// first-call trace next to the installed mod.  It is written only a handful of
// times and never on the steady-state path.
void diag(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (FILE* file = std::fopen(kDiagPath, "a")) {
        std::fputs(line, file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

// ---------------------------------------------------------------------------
// LeavesBlock::getRenderLayer hook (solid opaque leaves)
// ---------------------------------------------------------------------------

using GetRenderLayerFn = std::uint8_t (*)(void*, void*, void*, void*);
using GraphicsModeFn = void (*)(void*, void*);

struct LayerHook {
    GetRenderLayerFn original = nullptr;
    bedrocktools::hooks::Handle handle = nullptr;
    int kind = 0; // 0 = base/Mangrove (Fancy layer 10), 1 = Seasons/Cherry (7)
};

struct GraphicsHook {
    GraphicsModeFn original = nullptr;
    bedrocktools::hooks::Handle handle = nullptr;
};

LayerHook s_layerHooks[4]{};
int s_layerHookCount = 0;
std::atomic<void*> s_layerSelf[4]{};
int s_layerKind[4]{};
GraphicsHook s_graphicsHooks[4]{};
int s_graphicsHookCount = 0;

bool isTextPtr(std::uintptr_t p) {
    return p > 0x10000 && (p & 3) == 0;
}

bool isObjectPtr(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v > 0x10000 && (v & 3) == 0;
}

void forceLayerField(void* self) {
    if (!isObjectPtr(self)) return;
    *reinterpret_cast<std::uint8_t*>(static_cast<std::uint8_t*>(self) + kLayerFieldOffset) =
        kOpaqueRenderLayer;
}

void noteLayerCall(void* self, int index) {
    const int call = s_layerCalls.fetch_add(1, std::memory_order_relaxed);
    if (call >= 8) return;
    const auto fancy = isObjectPtr(self)
        ? *reinterpret_cast<const std::uint8_t*>(static_cast<const std::uint8_t*>(self) +
                                                 kFancyFieldOffset)
        : 0;
    diag("layer call #%d hook=%d self=%p fancy=%u -> %u", call + 1, index, self, fancy,
         static_cast<unsigned>(kOpaqueRenderLayer));
}

// A real getRenderLayer() override materializes at least one BlockRenderLayer
// constant (5..12) and consults the cached fancy/layer field.  The slot the
// game no longer uses for it is a short `mov w0,#1; ret` stub, and the base
// field getter reads neither field, so both fail and the module stays inert.
bool looksLikeLeafLayerFn(std::uintptr_t fn) {
    if (!isTextPtr(fn)) return false;
    const auto* code = reinterpret_cast<const std::uint32_t*>(fn);
    bool hasLayer = false;
    bool readsField = false;
    for (int i = 0; i < 96; ++i) {
        const auto w = code[i];
        if ((w & 0xFF800000u) == 0x52800000u) {          // movz wd, #imm
            const int imm = static_cast<int>((w >> 5) & 0xFFFFu);
            if (imm >= 5 && imm <= 12) hasLayer = true;
        } else if ((w & 0xFFC00000u) == 0x39400000u) {   // ldrb wt, [xn, #imm]
            const auto imm = (w >> 10) & 0xFFFu;
            if (imm == kFancyFieldOffset || imm == kLayerFieldOffset) readsField = true;
        }
        if (w == 0xD65F03C0u) break;                     // first ret ends the body
    }
    return hasLayer && readsField;
}

// onGraphicsModeChanged(Options*) caches the fancy-graphics state at +0x3a8
// and the render layer at +0x132.  Hooking it keeps the cached layer at Opaque
// for render paths that read the field instead of calling getRenderLayer().
bool looksLikeGraphicsModeFn(std::uintptr_t fn) {
    if (!isTextPtr(fn)) return false;
    const auto* code = reinterpret_cast<const std::uint32_t*>(fn);
    bool writesFancy = false;
    bool writesLayer = false;
    for (int i = 0; i < 64; ++i) {
        const auto w = code[i];
        if ((w & 0xFFC00000u) == 0x39000000u) {          // strb wt, [xn, #imm]
            const auto imm = (w >> 10) & 0xFFFu;
            if (imm == kFancyFieldOffset) writesFancy = true;
            else if (imm == kLayerFieldOffset) writesLayer = true;
        }
        if (writesFancy && writesLayer) return true;
        if (w == 0xD65F03C0u) break;
    }
    return false;
}

std::uint8_t forceLayer(int index, void* self) {
    forceLayerField(self);
    if (index >= 0 && index < 4) s_layerSelf[index].store(self, std::memory_order_release);
    noteLayerCall(self, index);
    return kOpaqueRenderLayer;
}

std::uint8_t layerDetour0(void* self, void* block, void* region, void* pos) {
    if (s_forceOpaque.load(std::memory_order_acquire)) return forceLayer(0, self);
    return s_layerHooks[0].original ? s_layerHooks[0].original(self, block, region, pos)
                                    : kOpaqueRenderLayer;
}

std::uint8_t layerDetour1(void* self, void* block, void* region, void* pos) {
    if (s_forceOpaque.load(std::memory_order_acquire)) return forceLayer(1, self);
    return s_layerHooks[1].original ? s_layerHooks[1].original(self, block, region, pos)
                                    : kOpaqueRenderLayer;
}

std::uint8_t layerDetour2(void* self, void* block, void* region, void* pos) {
    if (s_forceOpaque.load(std::memory_order_acquire)) return forceLayer(2, self);
    return s_layerHooks[2].original ? s_layerHooks[2].original(self, block, region, pos)
                                    : kOpaqueRenderLayer;
}

std::uint8_t layerDetour3(void* self, void* block, void* region, void* pos) {
    if (s_forceOpaque.load(std::memory_order_acquire)) return forceLayer(3, self);
    return s_layerHooks[3].original ? s_layerHooks[3].original(self, block, region, pos)
                                    : kOpaqueRenderLayer;
}

void* detourForIndex(int i) {
    if (i == 0) return reinterpret_cast<void*>(layerDetour0);
    if (i == 1) return reinterpret_cast<void*>(layerDetour1);
    if (i == 2) return reinterpret_cast<void*>(layerDetour2);
    return reinterpret_cast<void*>(layerDetour3);
}

void graphicsDetour0(void* self, void* options) {
    if (s_graphicsHooks[0].original) s_graphicsHooks[0].original(self, options);
    if (s_forceOpaque.load(std::memory_order_acquire)) forceLayerField(self);
}

void graphicsDetour1(void* self, void* options) {
    if (s_graphicsHooks[1].original) s_graphicsHooks[1].original(self, options);
    if (s_forceOpaque.load(std::memory_order_acquire)) forceLayerField(self);
}

void graphicsDetour2(void* self, void* options) {
    if (s_graphicsHooks[2].original) s_graphicsHooks[2].original(self, options);
    if (s_forceOpaque.load(std::memory_order_acquire)) forceLayerField(self);
}

void graphicsDetour3(void* self, void* options) {
    if (s_graphicsHooks[3].original) s_graphicsHooks[3].original(self, options);
    if (s_forceOpaque.load(std::memory_order_acquire)) forceLayerField(self);
}

void* graphicsDetourForIndex(int i) {
    if (i == 0) return reinterpret_cast<void*>(graphicsDetour0);
    if (i == 1) return reinterpret_cast<void*>(graphicsDetour1);
    if (i == 2) return reinterpret_cast<void*>(graphicsDetour2);
    return reinterpret_cast<void*>(graphicsDetour3);
}

bool alreadyHooked(std::uintptr_t fn) {
    for (int i = 0; i < s_layerHookCount; ++i) {
        if (s_layerHooks[i].handle &&
            reinterpret_cast<std::uintptr_t>(s_layerHooks[i].handle->target) == fn) {
            return true;
        }
    }
    return false;
}

bool alreadyHookedGraphics(std::uintptr_t fn) {
    for (int i = 0; i < s_graphicsHookCount; ++i) {
        if (s_graphicsHooks[i].handle &&
            reinterpret_cast<std::uintptr_t>(s_graphicsHooks[i].handle->target) == fn) {
            return true;
        }
    }
    return false;
}

void hookLeafType(const char* typeName, int kind) {
    std::uintptr_t fn = 0;
    std::size_t slot = 0;
    for (const std::size_t candidate : kRenderLayerSlots) {
        const auto resolved =
            pl::memory::resolveVtableFunction(typeName, candidate, "libminecraftpe.so");
        if (looksLikeLeafLayerFn(resolved)) {
            fn = resolved;
            slot = candidate;
            break;
        }
    }
    if (!fn) {
        VOLT_OPAQUE_LOG("getRenderLayer %s: no valid slot", typeName);
        diag("layer hook %s rejected", typeName);
        return;
    }
    if (alreadyHooked(fn) || s_layerHookCount >= 4) return;

    auto& hook = s_layerHooks[s_layerHookCount];
    hook.kind = kind;
    s_layerKind[s_layerHookCount] = kind;
    hook.handle = bedrocktools::hooks::install(
        reinterpret_cast<void*>(fn),
        detourForIndex(s_layerHookCount),
        reinterpret_cast<void**>(&hook.original));
    if (hook.handle) {
        VOLT_OPAQUE_LOG("getRenderLayer %s slot %zu = %p", typeName, slot,
                        reinterpret_cast<void*>(fn));
        diag("layer hook %s slot=%zu fn=%p", typeName, slot, reinterpret_cast<void*>(fn));
        ++s_layerHookCount;
    } else {
        VOLT_OPAQUE_LOG("getRenderLayer %s hook failed: %p", typeName, reinterpret_cast<void*>(fn));
        diag("layer hook %s FAILED fn=%p", typeName, reinterpret_cast<void*>(fn));
    }
}

void hookLeafGraphics(const char* typeName) {
    if (s_graphicsHookCount >= 4) return;
    std::uintptr_t fn = 0;
    std::size_t slot = 0;
    for (const std::size_t candidate : kGraphicsModeSlots) {
        const auto resolved =
            pl::memory::resolveVtableFunction(typeName, candidate, "libminecraftpe.so");
        if (looksLikeGraphicsModeFn(resolved)) {
            fn = resolved;
            slot = candidate;
            break;
        }
    }
    if (!fn) {
        VOLT_OPAQUE_LOG("onGraphicsModeChanged %s: no valid slot", typeName);
        diag("gfx hook %s rejected", typeName);
        return;
    }
    if (alreadyHookedGraphics(fn)) return;

    auto& hook = s_graphicsHooks[s_graphicsHookCount];
    hook.handle = bedrocktools::hooks::install(
        reinterpret_cast<void*>(fn),
        graphicsDetourForIndex(s_graphicsHookCount),
        reinterpret_cast<void**>(&hook.original));
    if (hook.handle) {
        VOLT_OPAQUE_LOG("onGraphicsModeChanged %s slot %zu = %p", typeName, slot,
                        reinterpret_cast<void*>(fn));
        diag("gfx hook %s slot=%zu fn=%p", typeName, slot, reinterpret_cast<void*>(fn));
        ++s_graphicsHookCount;
    } else {
        VOLT_OPAQUE_LOG("onGraphicsModeChanged %s hook failed: %p", typeName,
                        reinterpret_cast<void*>(fn));
        diag("gfx hook %s FAILED fn=%p", typeName, reinterpret_cast<void*>(fn));
    }
}

void restoreLayerFields() {
    for (int i = 0; i < s_layerHookCount; ++i) {
        void* self = s_layerSelf[i].load(std::memory_order_acquire);
        if (!isObjectPtr(self)) continue;
        const auto fancy = *reinterpret_cast<const std::uint8_t*>(
            static_cast<const std::uint8_t*>(self) + kFancyFieldOffset);
        const std::uint8_t layer = fancy ? (s_layerKind[i] == 1 ? 7 : 10) : 5;
        *reinterpret_cast<std::uint8_t*>(static_cast<std::uint8_t*>(self) + kLayerFieldOffset) =
            layer;
        diag("restore field hook=%d self=%p fancy=%u layer=%u", i, self, fancy, layer);
    }
}

void installLeafHooks() {
    hookLeafType("11LeavesBlock", 0);
    hookLeafType("26SeasonsAgnosticLeavesBlock", 1);
    hookLeafType("17CherryLeavesBlock", 1);
    hookLeafType("19MangroveLeavesBlock", 0);
    if (s_layerHookCount == 0) {
        VOLT_OPAQUE_LOG("leaf render-layer hook unavailable");
        diag("layer hooks unavailable");
    }
    hookLeafGraphics("11LeavesBlock");
    hookLeafGraphics("26SeasonsAgnosticLeavesBlock");
    hookLeafGraphics("17CherryLeavesBlock");
    hookLeafGraphics("19MangroveLeavesBlock");
    if (s_graphicsHookCount == 0) diag("gfx hooks unavailable");
}

// ---------------------------------------------------------------------------
// gfx_transparentleaves option: used to make the running game rebuild chunks.
// The render-layer hook above is authoritative for the actual leaf layer.
// ---------------------------------------------------------------------------

void* registryOption(void* registry) {
    if (!registry) return nullptr;
    auto* table = reinterpret_cast<void**>(reinterpret_cast<char*>(registry) + kRegistryOptionsOffset);
    return table[kTransparentLeavesOptionId];
}

std::uint8_t* optionValue(void* option);

void rememberOption(void* option) {
    if (!option) return;
    void* expected = nullptr;
    if (s_option.compare_exchange_strong(expected, option)) {
        VOLT_OPAQUE_LOG("option captured: %p", option);
        diag("option captured opt=%p value=%u", option,
             static_cast<unsigned>(*optionValue(option)));
    }
}

std::uint8_t* optionValue(void* option) {
    return reinterpret_cast<std::uint8_t*>(option) + kOptionValueOffset;
}

void* optionData(void* option) {
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(option) + kOptionDataOffset);
}

void forceLiveValue(void* option) {
    if (!option) return;

    // Remember the player's value before the first modification so disabling
    // the module can put the game back where it was.
    if (!s_savedValueValid.load(std::memory_order_acquire)) {
        s_savedValue.store(*optionValue(option) ? 1 : 0, std::memory_order_relaxed);
        s_savedValueValid.store(true, std::memory_order_release);
    }

    auto* value = optionValue(option);
    const bool changed = *value != 0;
    if (changed) *value = 0;

    // The option observers run on the render thread and touching them from
    // the menu thread crashed 1.26.60, so the value is only changed here.
    // Options.txt carries the same value into the next world load, and the
    // render-layer detours below are authoritative in the running world.
    (void)changed;
}

void restoreLiveValue(int forcedValue = -1) {
    void* option = s_option.load(std::memory_order_acquire);
    const bool hadSaved = s_savedValueValid.exchange(false, std::memory_order_acq_rel);
    if (!option) return;

    const int saved = forcedValue >= 0
        ? (forcedValue ? 1 : 0)
        : (hadSaved ? (s_savedValue.load(std::memory_order_relaxed) ? 1 : 0) : -1);
    if (saved < 0) return;

    auto* value = optionValue(option);
    const bool changed = *value != saved;
    if (changed) {
        *value = static_cast<std::uint8_t>(saved);
        VOLT_OPAQUE_LOG("restored live option to %d", saved);
    }
    // Do not notify observers here.  onDisable runs on the menu/UI thread,
    // while the option observers touch renderer state and must run on the
    // render thread; notifying from the menu crashed the game.  The live
    // getRenderLayer detours fall through immediately, and the remesh request
    // rebuilds the affected chunks with the player's real layer.
}

void* getOptionDetour(void* registry, std::uint32_t id) {
    void* option = s_getOptionOriginal ? s_getOptionOriginal(registry, id) : nullptr;

    if (registry) {
        void* discovered = registryOption(registry);
        if (discovered) {
            rememberOption(discovered);
            s_registry.store(registry, std::memory_order_release);
        } else if (!s_registry.load(std::memory_order_acquire)) {
            s_registry.store(registry, std::memory_order_release);
        }
    }

    if (id == kTransparentLeavesOptionId && option) {
        rememberOption(option);
        if (s_forceLive.load(std::memory_order_acquire)) forceLiveValue(option);
    }
    return option;
}

void installOptionHook() {
    if (s_getOptionHook) return;
    const auto getOptionAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::OptionRegistryGetOption);
    if (!getOptionAddress) {
        VOLT_OPAQUE_LOG("option hook unavailable; render-layer hook stays live");
        return;
    }
    s_getOptionHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(getOptionAddress),
        reinterpret_cast<void*>(getOptionDetour),
        reinterpret_cast<void**>(&s_getOptionOriginal));
    if (s_getOptionHook) VOLT_OPAQUE_LOG("option hook installed");
    else VOLT_OPAQUE_LOG("option hook failed; render-layer hook stays live");
}

// The launcher keeps the real options.txt under its media directory. Older
// installs and the standalone game use one of the other locations.
constexpr const char* kOptPaths[] = {
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/_shared/internal/games/com.mojang/minecraftpe/options.txt",
    "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/_shared/external/games/com.mojang/minecraftpe/options.txt",
    "/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt",
    "/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/games/com.mojang/minecraftpe/options.txt",
    "/storage/emulated/0/games/com.mojang/minecraftpe/options.txt",
    "/sdcard/games/com.mojang/minecraftpe/options.txt",
    "/sdcard/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt",
    "/data/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt",
};

std::filesystem::path backupPath(const std::filesystem::path& options) {
    return options.string() + ".volt-opaque-leaves";
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::filesystem::path& path, const std::string& data) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = std::filesystem::path(path.string() + ".volt.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

void uniqueOptionsPaths(std::vector<std::filesystem::path>& paths) {
    for (const char* candidate : kOptPaths) {
        std::filesystem::path path(candidate);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) continue;
        bool seen = false;
        for (const auto& existing : paths) {
            std::error_code equivEc;
            if (existing == path || (std::filesystem::equivalent(existing, path, equivEc) && !equivEc)) {
                seen = true;
                break;
            }
        }
        if (!seen) paths.push_back(std::move(path));
    }
}

// The sidecar is written before options.txt is changed, so it is the most
// trustworthy record of what the player had selected when the module was
// enabled.  It also survives a game restart that happens while the module is on.
bool readBackedUpValue(int& value) {
    std::vector<std::filesystem::path> paths;
    uniqueOptionsPaths(paths);
    for (const auto& path : paths) {
        std::string saved;
        if (!readFile(backupPath(path), saved)) continue;
        while (!saved.empty() &&
               (saved.back() == '\n' || saved.back() == '\r' || saved.back() == ' ' || saved.back() == '\t')) {
            saved.pop_back();
        }
        if (saved == "0") {
            value = 0;
            return true;
        }
        if (saved == "1") {
            value = 1;
            return true;
        }
    }
    return false;
}

void applyOpaqueLeaves() {
    std::vector<std::filesystem::path> paths;
    uniqueOptionsPaths(paths);
    if (paths.empty()) return;

    for (const auto& path : paths) {
        std::string original;
        if (!readFile(path, original)) continue;
        auto keys = parseOptions(original);
        const auto current = keys.find(kOptionKey);
        const std::string previous = current == keys.end() ? std::string("1") : current->second;

        const auto backup = backupPath(path);
        std::error_code ec;
        if (!std::filesystem::exists(backup, ec) || ec) {
            // Remember the player's value so disabling the module restores it,
            // even if the game rewrites options.txt while the module is active.
            writeFile(backup, previous);
        }

        if (current != keys.end() && current->second == "0") continue;
        keys[kOptionKey] = "0";
        const auto next = serializeOptions(original, keys);
        if (next != original) writeFile(path, next);
    }
}

void restoreOpaqueLeaves() {
    std::vector<std::filesystem::path> paths;
    uniqueOptionsPaths(paths);
    for (const auto& path : paths) {
        const auto backup = backupPath(path);
        std::string saved;
        if (!readFile(backup, saved) || saved.empty()) continue;

        std::string original;
        if (!readFile(path, original)) continue;
        auto keys = parseOptions(original);
        keys[kOptionKey] = saved;
        const auto next = serializeOptions(original, keys);
        if (next != original && !writeFile(path, next)) continue;
        std::error_code ec;
        std::filesystem::remove(backup, ec);
    }
}

} // namespace

OpaqueLeavesModule::OpaqueLeavesModule()
    : Module("Opaque Leaves",
             "Solid leaves in Fancy graphics, like Fast graphics.") {
    moduleId = "volt.opaqueleaves";
    masterEnabled = true;
    updateEnabledState();
}

void OpaqueLeavesModule::onInit() {
    static std::atomic<bool> diagReset{false};
    if (!diagReset.exchange(true, std::memory_order_acq_rel)) {
        if (FILE* file = std::fopen(kDiagPath, "w")) std::fclose(file);
    }
    s_forceLive.store(enabled, std::memory_order_release);
    s_forceOpaque.store(enabled, std::memory_order_release);
    installLeafHooks();
    installOptionHook();
    diag("init enabled=%d layerHooks=%d gfxHooks=%d option=%p", enabled ? 1 : 0,
         s_layerHookCount, s_graphicsHookCount, s_option.load(std::memory_order_acquire));
    for (int i = 0; i < s_layerHookCount; ++i) {
        diag("layer hook %d target=%p kind=%d", i,
             s_layerHooks[i].handle ? s_layerHooks[i].handle->target : nullptr, s_layerKind[i]);
    }
    for (int i = 0; i < s_graphicsHookCount; ++i) {
        diag("gfx hook %d target=%p", i,
             s_graphicsHooks[i].handle ? s_graphicsHooks[i].handle->target : nullptr);
    }

    if (enabled) {
        applyOpaqueLeaves();
        void* option = s_option.load(std::memory_order_acquire);
        if (!option) {
            void* registry = s_registry.load(std::memory_order_acquire);
            if (registry) option = registryOption(registry);
            rememberOption(option);
        }
        forceLiveValue(option);
    } else {
        s_savedValueValid.store(false, std::memory_order_release);
        restoreOpaqueLeaves();
    }
}

void OpaqueLeavesModule::onEnable() {
    s_savedValueValid.store(false, std::memory_order_release);
    s_layerCalls.store(0, std::memory_order_relaxed);
    s_forceLive.store(true, std::memory_order_release);
    s_forceOpaque.store(true, std::memory_order_release);
    installLeafHooks();
    applyOpaqueLeaves();
    diag("enable layerHooks=%d gfxHooks=%d", s_layerHookCount, s_graphicsHookCount);
    forceLiveValue(s_option.load(std::memory_order_acquire));
}

void OpaqueLeavesModule::onDisable() {
    // Let the getRenderLayer detours fall through before the option-driven
    // rebuild, so the rebuilt chunks keep the player's original layer.
    diag("disable layerCalls=%d", s_layerCalls.load(std::memory_order_relaxed));
    s_forceOpaque.store(false, std::memory_order_release);
    s_forceLive.store(false, std::memory_order_release);
    restoreLayerFields();
    int original = -1;
    const bool haveOriginal = readBackedUpValue(original);
    restoreLiveValue(haveOriginal ? original : -1);
    restoreOpaqueLeaves();
}
