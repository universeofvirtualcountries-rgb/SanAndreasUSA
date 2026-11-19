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

Newer installs are 64-bit only; the `armeabi-v7a` library is still shipped for 2.00.

## 3. Unpack

```
/sdcard/AMLMods/com.rockstargames.gtasa/USA1to1/
├── lib/arm64-v8a/libUSA1to1.so      (or lib/armeabi-v7a/…)
├── assets/
│   ├── manifest.json
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
3. **Two-finger swipe down** anywhere on the screen opens the overlay.
4. `Polygon` tab → *Start polygon* spawns the endless road with a yellow marker
   every 200 m. Drive it and watch `Origin shifts` in the telemetry tab climb.
5. `Teleports` tab → jump to New York / Washington / Las Vegas / Los Angeles.
6. `Missions` tab → start a JSON mission. The state is saved in `world_delta.json`.

## 5. Optional: exact collision data

```bash
# extract .ide/.col from your game archives first (IMG Tool / fastman92 tools)
python3 tools/parse_ide.py --game /path/to/extracted/GTA_SA --out objects_db.json
adb push objects_db.json /sdcard/AMLMods/com.rockstargames.gtasa/USA1to1/assets/
```

Without that file the mod scans the game's own `CModelInfo` at runtime and writes
`assets/objects_db_live.json` so you can see what it found.

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

* **No overlay** — check the AML log (`logcat | grep USA1to1`) for
  `Overlay: ImGui RenderWare backend failed to initialise`; the mod keeps running
  headless and streaming still works.
* **Nothing spawns** — the catalogue is empty: see section 5. Markers still appear.
* **Game stutters when moving fast** — lower `streaming.max_chunks_per_update` /
  `max_spawns_per_update` in `config.json`.
* **Wrong props in buildings** — your `objects_db.json` came from another game/mod;
  regenerate it from your own files.
