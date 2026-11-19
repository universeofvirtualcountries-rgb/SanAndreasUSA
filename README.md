# USA 1:1 Procedural Expansion

A modular GTA: San Andreas Android mod (AML + Plugin-SDK) that turns the map into a
1:1 scale, procedurally streamed slice of the continental United States: ~4,500 km wide,
9.8 million km² of empty plane that gets filled with chunked streets, deterministic
interiors and JSON missions while the engine only ever sees ±1000 m of local space.

* **ABIs:** `armeabi-v7a` (GTA SA 2.00) and `arm64-v8a` (GTA SA 2.10+)
* **Loader:** Android Mod Loader (AML), mod folder `/sdcard/AMLMods/<game>/USA1to1/`
* **Target:** NDK r27d, C++17, `c++_static`, no OpenSSL / libcurl

---

## What is actually inside

| Area | Implementation |
| --- | --- |
| Scale & floating origin | `src/core/floating_origin.*` — global coords in `double`, chunk-quantised shifts, camera + vehicle + peds + our objects all moved, velocities preserved |
| Streaming | `src/core/chunk_streamer.*` — 3×3 / 5×5 grid of 200×200 m chunks, per-frame budgets, retries props until models load, unloads objects, markers and models |
| Determinism | `src/core/hash.*` — splitmix64/FNV seeds from global chunk coordinates, `std::mt19937_64` streams; same door → same apartment, forever |
| Terrain | `src/core/heightfield.*` — C0-continuous heightfield, seam-exact chunk edges (unit tested) |
| Interiors | `src/core/interior.*` — 3–7 connected rooms at `z ≥ 1000`, furniture, cash register, exit marker; EnEx markers are real yellow `C3dMarkers` |
| Saves | `src/core/world_delta.*` — compact `world_delta.json` with only player state, robbed tills and owned houses |
| Missions | `src/core/mission.*` + `assets/missions/*.json` — no recompilation needed, hot-swappable |
| Cloud | `src/net/cloud_sync.*` + `src/net/android_http.*` — `java.net.HttpURLConnection` over JNI, manifest + hash verification, background thread |
| UI | `src/ui/imgui_overlay.*` — Dear ImGui through AML's RenderWare backend, opened by a two-finger swipe down |
| Engine bridge | `src/adapter/gtasa_adapter.*` — the only file that touches `libGTASA.so` |
| Trust boundary | `src/core/safe.hpp` — validates every pointer/string from AML, JNI and the game image (a stale loader slot returns `0x7`, not `nullptr`) |
| Tools | `tools/parse_ide.py` (.ide/.col → `objects_db.json`), `tools/arm_compile_check.sh`, `tools/check_source_lists.py` (every .cpp must be in CMakeLists + Android.mk) |
| CI | `ci/build_android.yml` — host tests, both ABIs (NDK r27d), packaged `USA1to1.zip` |

---

## Build

### Host (unit tests, no Android needed)

```bash
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j
./build-host/usa1to1_tests
```

38 cases / ~36k assertions cover: JSON, seeding, heightfield seams + chunk edge
contract, floating origin (invariants, velocity preservation, teleport, interiors),
streaming (load/unload/retry/door lookup), interior determinism + robbery, delta saves,
mission parsing and playthrough, cloud sync (success/hash-mismatch/offline), gesture
recognition, test polygon, config clamps and the region table.

### Android

CI is the authoritative build (`ci/build_android.yml`, NDK r27d — copy it to
`.github/workflows/` once, see [`ci/INSTALL_CI.md`](ci/INSTALL_CI.md); the branch's
GitHub App token is not allowed to push workflow files itself):

```bash
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static
cmake --build build-arm64 -j
```

`ndk-build` users get the mirrored `Android.mk` / `Application.mk`.

Cross-ABI code check without the NDK (both 64- and 32-bit ARM codegen):

```bash
ZIG=/path/to/zig tools/arm_compile_check.sh
```

---

## Install on a phone

1. Install AML and GTA SA 2.00 / 2.10.
2. Unzip `USA1to1.zip` into `/sdcard/AMLMods/com.rockstargames.gtasa/USA1to1/`
   so that you get `lib/arm64-v8a/libUSA1to1.so` (or `armeabi-v7a`), `assets/` and
   `config.json` is created on first run.
3. Launch the game, load any save. The mod boots in two stages: loader stage
   (`OnModLoad`) and engine stage (first game tick with a live player), so a
   slow or old loader cannot crash it during startup.
4. **Two-finger swipe down** opens the menu.

Details: [`docs/MOD_INSTALL.md`](docs/MOD_INSTALL.md).
Engine integration report: [`docs/ADAPTER.md`](docs/ADAPTER.md).
Design notes: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Honest limitations

* Procedural content needs a model catalogue. Without `assets/objects_db.json` the mod
  resolves the bundled model whitelist against live `CModelInfo` and validates
  numeric fallback candidates; running `tools/parse_ide.py` improves the tags.
  If neither finds floor/wall models, interiors are furniture-only — no fake geometry
  is invented.
* `parse_ide.py` does not guess COL4 bounds (they live in sub-sections and differ
  between tools); such models are reported, not silently mis-measured.
* R*'s original map is still loaded by the game. This mod builds its own world next to
  it; disabling the vanilla map/ide loading is a separate, much more invasive patch.
* Telemetry/paths assume the mod folder layout above.

MIT-licensed third-party code is listed in [`third_party_licenses/`](third_party_licenses).
