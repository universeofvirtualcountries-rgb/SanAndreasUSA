# libGTASA.so integration report

Everything below lives in `src/adapter/gtasa_adapter.cpp` + `gtasa_adapter.hpp`.
The adapter is the **only** file that includes `plugin-sdk` and `mod/amlmod.h`;
the rest of the mod talks to the engine through `core::IEngineBridge`, which is why
every system has host unit tests.

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

## 2. Hooks

| Hook | Symbol | Purpose |
| --- | --- | --- |
| `CGame::Process` | `_ZN5CGame7ProcessEv` | per-frame tick: floating origin, streaming, missions, cloud poll |
| `CGame::Shutdown` | `_ZN5CGame8ShutdownEv` | teardown: save delta, destroy objects, hide markers, ImGui shutdown |
| `AND_TouchEvent` | `_Z14AND_TouchEventiiii` | two-finger swipe gesture for the overlay |
| RenderWare frame | AML ImGui backend (`ImGui_ImplRenderWare_Init`) | draws the overlay between `NewFrame()` and `Render()` |

Hooks are installed with `aml->Hook(target, replacement, &original)`; the trampoline
always calls the original afterwards so the game keeps running normally.

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

* `CStreaming::RequestModel(id, 0)` to request, `CStreaming::ms_aInfoForModel[id].m_status == 1`
  to test readiness (bounds-checked against the mobile limit of 26316 ids),
  `CStreaming::SetModelIsDeletable(id)` to release.
* Spawn: `CObject::Create(modelId, true)` → `bUsesCollision = 1` → `CWorld::Add(obj)` →
  `Teleport(pos)` → `CMatrix::SetRotateZOnly(heading)` → `UpdateRwFrame()`.
* Despawn: `CWorld::Remove(obj)` → native destructor via the plugin-sdk shim →
  `CPools::ms_pObjectPool->Delete(obj)` → `SetModelIsDeletable(modelId)`.
  The slot is validated with `CPools::ms_pObjectPool->IsObjectValid()` first, so a
  slot already reclaimed by the game (restart, interior change) never gets double-freed.
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

`BuildLiveCatalog()` walks `CModelInfo::ms_modelInfoPtrs[400..26316]`, skips LODs,
vehicles and zero-draw-distance entries, reads `m_szModelName`, `m_fDrawDistance`,
`bIsRoad` and the collision bound sphere (`m_pColModel->m_boundSphere`) and classifies
models into the generator's tags. `tools/parse_ide.py` produces the same schema with
exact box bounds when the user runs it on extracted game files.

## 8. Version differences handled

| Concern | 2.00 (`armeabi-v7a`) | 2.10+ (`arm64-v8a`) |
| --- | --- | --- |
| Identifier width in marker/object symbols | `j` (32-bit) via `BYBIT` | `y` (64-bit) via `BYBIT` |
| Addresses for PLT-only symbols | 32-bit offsets from the plugin-sdk table | 64-bit offsets |
| `AAssetManager` / JNI | attached through AML (`GetJNIEnvironment`, `GetJavaVM`) | same |

The adapter never hardcodes an address for anything the mobile build exports.

## 9. Verification status

* Host: `usa1to1_tests` — 38 cases, ~36,600 assertions, all green.
* Cross-ABI: `tools/arm_compile_check.sh` compiles **all 26 translation units**
  (mod + ImGui + plugin-sdk + core) for `aarch64` and `armv7` with clang, catching
  32/64-bit branch and ABI mistakes without the NDK.
* Linking against `libGTASA.so`, `liblog`, `libandroid` and `libc++_static` happens in
  GitHub Actions with NDK r27d — that is the authoritative `.so` (nothing else can
  produce a loadable AML module).

## 10. What to check first in a device log

```
Adapter: libGTASA.so @ 0x..., version 2.10+ arm64-v8a
Adapter: hooked CGame::Process at 0x...
Adapter: road model from bIsRoad flag = ...
USA 1:1 loaded: N models, 2 missions, grid 5x5
FloatingOrigin: shift (-400.0, 0.0) entities=...
```

If a hook line is missing, that export is absent in that game build: everything else
still loads, and the log says exactly which symbol failed.
