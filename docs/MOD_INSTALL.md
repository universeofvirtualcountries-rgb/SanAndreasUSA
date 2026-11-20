# Install & first run

## 1. Get the binaries

**GitHub Actions → the `Build libUSA1to1.so` workflow → latest green run → Artifacts.**

* `USA1to1-full` — one zip with both ABIs + assets. **Use this one.**
* `USA1to1-arm64-v8a` / `USA1to1-armeabi-v7a` — per-ABI artifacts if you build only one.

The workflow runs on every push to `main`, on pull requests, and manually
(*Run workflow*). The host test job must pass before the Android build starts.

If the *Actions* tab has no `Build libUSA1to1.so` workflow yet, enable it once via
[`../ci/INSTALL_CI.md`](../ci/INSTALL_CI.md) (copy `ci/build_android.yml` to
`.github/workflows/`), or build locally with the NDK as shown in the README.

## 2. Pick the right ABI

| Game version | CPU | ABI |
| --- | --- | --- |
| GTA SA **2.00** | 32-bit installs | `armeabi-v7a` |
| GTA SA **2.10+** | 64-bit installs | `arm64-v8a` |

Use **AML 1.3.0 or newer**. Newer game installs are 64-bit only; the `armeabi-v7a` library is still shipped for 2.00.

## 3. Unpack

```
/sdcard/AMLMods/com.rockstargames.gtasa/USA1to1/
├── lib/arm64-v8a/libUSA1to1.so      (or lib/armeabi-v7a/…)
├── assets/
│   ├── manifest.json
│   ├── model_whitelist.json
│   ├── missions/mission_001_robbery.json
│   ├── missions/mission_002_presidential_hit.json
│   └── objects_db.sample.json
└── README_INSTALL.md
```

AML loads shared libraries from the mod folder automatically. If your AML version
expects a flat layout, putting `libUSA1to1.so` directly in the mod folder also works —
the mod does not care where it was loaded from, only where its JSON lives.

Grant storage permission to the game, otherwise the mod cannot write
`world_delta.json` / `config.json`.

## 4. First run

1. Launch GTA SA through AML, load a save.
2. The mod teleports you to **Los Angeles** on the 1:1 plane (configurable:
   `content.spawn_city` in `config.json`).
3. Toggle the overlay with **three quick taps** (≤600 ms) or a **two-finger
   swipe up or down** (60 screen pixels). When closed, you can also tap the
   **[USA 1:1]** pill at top centre or the top-left shortcut. These shortcuts
   do not depend on ImGui mouse input.
4. `Polygon` tab → *Start polygon* spawns the endless road with a yellow marker
   every 200 m at `content.generation_z` (300 m by default, clamped to 50–900).
   You start 2 m above a ready road; if the tile underneath is missing, the
   mod pins you at `generation_z + 1.5` until its collision is ready. Drive
   it and watch `Origin shifts` in Telemetry. The banner and Telemetry show
   `Catalog: … | RoadID: … | Spawned: … | Err: …`.
5. `Teleports` tab → jump to New York / Washington / Las Vegas / Los Angeles.
6. `Missions` tab → start a JSON mission. The state is saved in `world_delta.json`.
7. Short status messages ("Entering shop...", "Mission complete", "+$2500")
   appear as a centred banner for 3 seconds and in the telemetry tab - they are
   rendered by the overlay, not by AML toasts (those abort on Android 15).

## 5. Optional: exact collision data

```bash
# extract .ide/.col from your game archives first (IMG Tool / fastman92 tools)
python3 tools/parse_ide.py --game /path/to/extracted/GTA_SA --out objects_db.json
adb push objects_db.json /sdcard/AMLMods/com.rockstargames.gtasa/USA1to1/assets/
```

The mod resolves database and bundled whitelist names against the live game,
then uses validated numeric candidates (3000–18000) as a fallback.
`assets/objects_db_live.json` records what the game actually accepted.

## 6. Files the mod writes

| File | Content |
| --- | --- |
| `config.json` | settings (created with defaults on first run) |
| `world_delta.json` | player global position, robbed tills, owned houses, mission progress, money |
| `cloud_state.json` | versions/hashes of cloud-synced missions |
| `assets/objects_db_live.json` | dump of the runtime model scan (debug aid) |

Delete `world_delta.json` for a fresh start. Delete `cloud_state.json` to force a
full mission re-sync.

## 7. Troubleshooting

* **Crash right after "USA 1:1 Procedural Expansion loading (loader stage)..."**
  — the log line order tells the stage: `mod dir:` (paths), `config loaded`,
  `Adapter: libGTASA.so base = 0x...` (loader), `Adapter: hooks installed`
  (hooks), `Adapter: N/12 engine symbols` (engine exports), `USA 1:1 loaded`
  (runtime). Whichever line is missing is the stage that failed; every failure
  path now logs the raw pointer it rejected instead of crashing.
* **Loader older than the API the mod is built against** — the mod targets AML
  builds that provide `GetLibHandle`/`GetSym`; on older loaders the log says
  `loader does not provide a libGTASA.so handle` and the mod stays inert instead
  of taking the game down.
* **No overlay** — check the AML log (`logcat | grep USA1to1`) for
  `Overlay: ImGui RenderWare backend failed to initialise`; the mod keeps running
  headless and streaming still works.
* **Taps miss** — open the menu and tap: the top status line and `logcat` show
  `Tap raw=(x,y) -> imgui=(ix,iy) hovered=...`. The `touch raw … -> imgui`
  line in Telemetry describes the shared input path. Use the `Touch event
  received` message to confirm a raw shortcut fired. By default
  (`ui.screen_width` and `ui.screen_height` both `0`), the backend uses the
  EGL draw surface, a zero-origin `GL_VIEWPORT`, or `RsGlobal`, in that order.
  If those sizes differ from Android's touch coordinates, set **both** axes
  in `config.json` (e.g. add `"screen_width": 2400, "screen_height": 1080`
  to the `"ui"` object) and restart the game.
* **Nothing spawns** — first note the `Catalog: … | RoadID: … | Spawned: … | Err: …`
  banner. In the log, look for `Adapter: catalogue=…`, then the rejection counts
  and the first eight `ms_modelInfoPtrs` slots if the catalogue is empty. Compare
  `requested`, `loaded`, and `failed` in the `ChunkStreamer` and `TestPolygon` logs.
  Keep `content.model_id_limit` at the actual table size (stock default 20000,
  up to 26316 for supported limit-adjusted setups).
* **Game stutters when moving fast** — lower `streaming.max_chunks_per_update` /
  `max_spawns_per_update` in `config.json`.
* **Unexpected props in buildings** — regenerate `objects_db.json` from your
  own game files to improve tags over the live name heuristics.
