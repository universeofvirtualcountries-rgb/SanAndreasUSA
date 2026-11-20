# libGTASA.so integration report

Everything below lives in `src/adapter/gtasa_adapter.cpp` + `gtasa_adapter.hpp`.
The adapter is the **only** file that includes `plugin-sdk` and `mod/amlmod.h`;
the rest of the mod talks to the engine through `core::IEngineBridge`, which is why
every system has host unit tests.

## 0. Boot sequence (three stages)

The mod never assumes the engine is ready when the loader calls it. On a real
device (AML 1.3.0, Android 15, GTA 2.00) touching engine state from `OnModLoad`
is a SIGSEGV, and the loader itself is a version-dependent vtable that can
return garbage for slots it does not implement.

| Stage | Entry point | What runs | What must not run |
| --- | --- | --- | --- |
| 1. Loader | `OnModLoad` | logger sink, mod directory, `config.json`, save/missions JSON, `libGTASA.so` handle, hooks, symbol table verification | anything reading engine arrays |
| 1b. All mods | `OnAllModsLoaded` | retry of the loader stage if another mod was still loading | engine work |
| 2. Runtime | first `CGame::Process` tick **where the player ped exists** | `CModelInfo` scan, road probe, streaming, interiors, ImGui, JNI/CloudSync | - |

`IsEngineLoaded()` is the gate: `FindPlayerPed(0) != nullptr` plus verified
`CStreaming`/`FindPlayerPed` symbols. During the loading screen the model info
array is still filling, so scanning then would produce an empty catalogue.

## 0.1 Trust boundary

`src/core/safe.hpp` is applied to everything that crosses the loader/JNI/game
boundary:

* `IsSanePointer` rejects `nullptr` and anything below `0x10000` - this is what
  catches a stale AML slot returning `0x7`;
* `IsMappedRange` probes the actual page table with `mincore(2)` before a
  dereference (a large stale pointer is rejected too, not just a small one);
* `IsReadableCString` / `IsSanePath` are used for every `const char*` from AML;
* `GTASAAdapter::Symbol()` validates every `aml->GetSym` result and disables the
  feature instead of calling a null pointer;
* `AndroidHttpTransport::Available()` validates the `JavaVM`/`JNIEnv` interface
  pointers before any JNI call;
* `IsUsableTableEntry` guards dense-but-not-really engine tables: the slot has to
  be mapped **and** the pointer it holds has to cover a whole structure. Stock
  2.00 leaves `CModelInfo::ms_modelInfoPtrs` slots holding plain integers, so
  `info->m_szModelName[0]` on slot value `10184` faulted at `0x27D0` — that is
  now a skip plus a log line.

The scan never goes past `content.model_id_limit` (default **20000**, ceiling
26316 for Fastman92-remapped ids). If no models pass validation, the log gives
rejection counts by reason and dumps the first eight raw slots in hex.

## 1. Symbol resolution

```cpp
m_libHandle = aml->GetLibHandle("libGTASA.so");            // handle for aml->GetSym
void *addr  = aml->GetSym(m_libHandle, "_ZN5CGame7ProcessEv");
```

* `aml->GetSym` is called with **mangled** mobile export names, exactly like AML's
  own plugin-sdk does (`plugin-sdk/sdk_base.h` wraps this in `GetMainLibrarySymbol`).
* The bundled mobile plugin-sdk (`plugin-sdk/game_sa/**`) is used for the game ABI:
  it resolves each function/global lazily through the same handle and carries the
  `BYBIT()` 32/64-bit address pairs for the few PLT-only symbols.
* Every symbol is resolved defensively: a missing export logs a warning and the
  related feature degrades instead of crashing.

The startup and engine adapter use only early `IAML` slots. From the vtable
address point (zero-based), these are `GetCurrentGame` (0), `GetConfigPath` (1),
`GetLib` (4), `GetSym(void *, ...)` (5), `Hook` (6), `GetDataPath` (14), and
`GetLibHandle(const char *)` (28); the slots are the same on ARMv7 and ARM64.
The optional CloudSync transport separately uses `GetJavaVM`, part of AML 1.3.0.

The mod does not read display dimensions through the loader. After a render
context is current, the overlay chooses the configured touch size or the EGL
draw surface / `GL_VIEWPORT` / `RsGlobal` fallback. The coordinator publishes
screen and render dimensions together under its mutex; the input hook does not
query EGL or GLES. `RsGlobal` still defines ImGui's rendering coordinate space.

## 1.1 No AML JNI facades

`aml->ShowToast()` and `aml->DoVibro()` are JNI facades: AML keeps a `JNIEnv`
cached from the Java/UI thread and calls `CallVoidMethod` from whatever thread
the mod happens to run on. On Android 15 / ART that aborts the process inside
`art::JavaVMExt::JniAbort` (`Toast` also wants a `Looper`). The mod therefore
never calls them:

* `GTASAAdapter::ShowToast()` logs the message and pushes it into
  `core::StatusFeed`; the ImGui overlay renders it (a 3 s centred banner and a
  `status:` line in the telemetry tab).
* `GTASAAdapter::Vibrate()` is a no-op with a debug log.

Our own JNI use (CloudSync) is different: it attaches the worker thread with
`AttachCurrentThread` and validates the `JavaVM`/`JNIEnv` interface tables first.

## 2. Hooks

| Hook | Symbol | Purpose |
| --- | --- | --- |
| `CGame::Process` | `_ZN5CGame7ProcessEv` | per-frame tick: floating origin, streaming, missions, cloud poll |
| `CGame::Shutdown` | `_ZN5CGame8ShutdownEv` | teardown: save delta, destroy objects, hide markers, ImGui shutdown |
| `AND_TouchEvent` | `_Z14AND_TouchEventiiii` | direct menu input, pill routing and swipe gesture |
| RenderWare frame | AML ImGui backend (`ImGui_ImplRenderWare_Init`) | draws the overlay between `NewFrame()` and `Render()` |

Hooks are installed with `aml->Hook(target, replacement, &original)`. The
adapter is this mod's only touch hook; it forwards unclaimed events to the game.
Three quick taps (600 ms) or a two-finger vertical swipe (60 raw px in
both directions) toggle the menu independently of ImGui. When closed, the hook
also owns the top-centre 25–75% × 0–12% hitbox and the top-left shortcut.
While open it routes ordinary raw touches to a render-thread queue, applies
`ScreenToRender` and reports `Tap raw=(x,y) -> imgui=(ix,iy) hovered=...`.

## 3. Player, vehicle, camera

* `FindPlayerPed(0)` / `FindPlayerVehicle(0, false)` (`_Z13FindPlayerPedi`, `_Z17FindPlayerVehicleib`).
* Position: `CPlaceable::GetPosition()`; movement uses the virtual
  `CEntity::Teleport(CVector, resetRotation=false)` and `UpdateRwFrame()`.
* **Velocity is preserved explicitly**: `CPhysical::m_vecMoveSpeed` is saved before
  `Teleport` and written back after, for the player, the player's vehicle and every
  pool entity we shift.
* Camera: all three `CCamera::m_aCams[]` slots get positional fields shifted
  (`m_vecSource`, `m_vecSourceBeforeLookBehind`, `m_vecCamFixedModeSource`,
  `m_vecTargetCoorsForFudgeInter`, target history) plus the camera-level interpolation
  state (`m_vecSourceWhenInterPol`, `m_vecTargetWhenInterPol`, `m_vecSourceDuringInter`,
  `m_vecTargetDuringInter`, `m_vecGameCamPos`, `m_vecFixedModeSource`,
  `m_vecAimingTargetCoors`, `m_vecAttachedCamOffset`). Directions are never touched.

## 4. Streaming and object lifecycle

* `CStreaming::RequestModel(id, 0)` queues the model. A spawn requires streaming
  status `1` **and** mapped `m_pRwObject` / `m_pColModel` in the live model slot.
  The adapter re-reads the GOT indirection (which FLA may relocate) rather than
  keeping catalogue-time model pointers. Chunks retry asynchronously.
* For each polygon candidate, `RequestModel(id, 2)` is followed by one
  `LoadAllRequestedModels(false)`; construction starts on a later tick. When the native COL slot is identifiable,
  the adapter also references it so ordinary collision streaming can load and
  retain it outside the slab's original city sector. There is no blocking
  retry per tile. The previous candidate and its COL reference are released
  before advancing to the next one.
* The typed `_ZN7CObject6CreateEib(int, bool)` call is gated by
  `IsModelLoaded`, with `true` for road slabs and `false` for other props. A
  road returned without its own mapped RW instance is rejected, not repaired.
  The manual `CreateRwObject` fallback is only for ordinary `Create(false)` props.
* Immediately before `CWorld::Add` the adapter rechecks load status, the live
  model RW/COL, at least 16 bytes of instance RW, its frame, the entity's
  effective `GetColModel()` and finite bounds. The preflight also checks physical
  type, repeat-sector storage and capacity in both link pools. Rejected
  allocations are destroyed without `CWorld::Remove`; a structurally rejected
  slab advances to the next candidate without trying it on other tiles.
  Object `Teleport` checks effective COL/RW too, as it can re-add an object.
* `Road Add begin` / `Road Add returned` distinguish a crash inside the native
  insertion from one on a later game tick. These checks do not establish which
  pointer was null in a prior crash. Telemetry reports `Catalog: N | RoadID: X |
  Loaded: yes/no | Spawned: Z | Err: …`.
* **Spawn Car** (Polygon or Teleports) queues one Sultan (560) on the game tick.
  It waits for nearby solid ground and a loaded vehicle model, uses native
  vehicle allocation and the automobile constructor, then runs the same
  preflight. A missing export, incompatible model or timed-out request is shown
  in StatusFeed; the streaming request is released on completion or failure.
* Despawn: `CWorld::Remove(obj)` → native destructor via the plugin-sdk shim →
  `CPools::ms_pObjectPool->Delete(obj)`. The request owner releases the model
  through `CStreaming::SetModelIsDeletable(id)` when it unloads.
* Markers: `C3dMarkers::PlaceMarker(...)` with type `MARKER3D_CYLINDER`, yellow
  (255, 214, 0), alpha 200. Slots are recycled from a fixed pool of 12 engine ids —
  the engine marker array is tiny and its size differs between game versions, so the
  adapter never indexes it directly. Removal hides the slot under the map and frees
  the id for immediate reuse.

## 5. Pool iteration (floating origin)

`CPools::ms_pPedPool`, `ms_pVehiclePool`, `ms_pObjectPool` are walked with
`GetSize()`/`GetAt(i)`. Skipped on purpose:

* the player ped and the player's vehicle (they are shifted by dedicated calls, first),
* anything at `z >= 1000` (`IsInteriorZ`) or with `m_nInterior != 0` — interior pockets
  belong to another coordinate pocket and must not move with the map,
* objects created by this mod are shifted once (tracked by pointer in `m_owned`).

## 6. Interiors

`CGame::currArea` + `CPlayerPed::m_nInterior` are set to the pocket's area id
(`1..17`); the pocket itself lives at `z = 1000 + area*60`. Because the floating origin
skips interior Z, the main world offset is untouched while the player is inside.

## 7. Model catalogue

`BuildLiveCatalog()` resolves `objects_db.json` and `model_whitelist.json` by
name via `CModelInfo::GetModelInfo(const char *, int *)`. JSON IDs are not used
to spawn objects. The numeric fallback is limited to 3000–18000 and the
configured table limit. Both paths require a matching static-model type, a
bounded model name, a valid draw distance and a collision box. The road is
selected from the native road-flagged table entries, then the whitelist, the
`RoadFlat` bucket, and finally the largest suitable flat collision footprint.
The default range is **3000–18000**, with **18450** as a single explicit
exception. If the primary road fails, try only the live-validated stock slab
candidates `3095`, `8558`, `6959`, `18450` in that order. All candidates exclude
`dyn_` and `object.dat` entries and require flat collision above 25 m²; normal
catalogue entries additionally need a road name hint or native road flag.
Barrels and crates are not road candidates. `Catalog: … | RoadID: … | Loaded: … | Spawned: … |
Err: …` shows the selection, instance count and failing stage.

The polygon uses a flat `content.generation_z`; chunk props and markers use
that base plus deterministic terrain relief. The polygon moves the player to
`generation_z + 2` on entry and after an origin shift, and recovers a fall
below `generation_z - 5` while active. Until the actual supporting tile is
ready, it pins the player (and player vehicle, if any) at `generation_z + 1.5`
and zeros linear velocity. It checks that a downward collision query at the
player's XY hits the supporting slab; other tiles do not release the hold.

## 8. Version differences handled

| Concern | 2.00 (`armeabi-v7a`) | 2.10+ (`arm64-v8a`) |
| --- | --- | --- |
| Identifier width in marker/object symbols | `j` (32-bit) via `BYBIT` | `y` (64-bit) via `BYBIT` |
| Addresses for PLT-only symbols | 32-bit offsets from the plugin-sdk table | 64-bit offsets |
| `AAssetManager` / JNI | attached through AML (`GetJNIEnvironment`, `GetJavaVM`) | same |

The adapter never hardcodes an address for anything the mobile build exports.

## 8.1 Crash triage

The device build now ships with `-g1 -funwind-tables` and a linker map, and
`ON_GAME_CRASH` logs the library, signal, code and offset. To resolve a trace:

```bash
# artifacts/USA1to1-arm64-v8a/symbols/<abi>-symbols.txt is sorted by address
awk '$1 <= "000afa66"' arm64-v8a-symbols.txt | tail -3
# or, with the .so and the map from the same artifact:
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-addr2line -f -C -e libUSA1to1.so 0xafa66
grep -n "0x00000000000afa" arm64-v8a-USA1to1.map | head
```

Because the mod is linked with `--exclude-libs,ALL`, plugin/ImGui symbols stay
private; `llvm-nm -C` in the artifact dump is `-C`'d, so the names are readable.

## 9. Verification status

* Host: `usa1to1_tests` — 38 cases, ~36,600 assertions, all green.
* Cross-ABI: `tools/arm_compile_check.sh` compiles **all 27 translation units**
  (mod + ImGui + plugin-sdk + core) for `aarch64` and `armv7` with clang, catching
  32/64-bit branch and ABI mistakes without the NDK.
* Linking against `libGTASA.so`, `liblog`, `libandroid` and `libc++_static` happens in
  GitHub Actions with NDK r27d — that is the authoritative `.so` (nothing else can
  produce a loadable AML module).

## 10. What to check first in a device log

```
Adapter: libGTASA.so @ 0x..., version 2.10+ arm64-v8a
Adapter: hooked CGame::Process at 0x...
Adapter: catalogue=N db=... whitelist=... numeric=... limit=... road=...
ChunkStreamer: requested=... loaded=... failed=... spawn_failed=...
USA 1:1 loaded: N models, 2 missions, grid 5x5
FloatingOrigin: shift (-400.0, 0.0) entities=...
```

If a hook line is missing, that export is absent in that game build: everything else
still loads, and the log says exactly which symbol failed.
