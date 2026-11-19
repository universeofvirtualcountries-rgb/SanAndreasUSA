# Architecture

```
src/main.cpp                 wires everything together (ModApp), AML entry points
├── src/adapter/             the ONLY code that knows libGTASA.so exists
│   └── gtasa_adapter.cpp    hooks, pools, streaming, markers, live catalogue scan
├── src/core/                engine-neutral, host-tested logic
│   ├── types.hpp            Vec2d/Vec3f, interior Z pocket, limits
│   ├── hash.*               splitmix64/FNV seeds, mt19937_64 streams
│   ├── json.*               dependency-free JSON (delta, missions, manifest, db)
│   ├── regions.*            US plane, state envelopes, city anchors, sectors
│   ├── heightfield.*        C0-continuous terrain, seam-exact chunk edges
│   ├── model_catalog.*      real model ids/tags/collision from JSON or live scan
│   ├── chunk_generator.*    deterministic 200 m chunk content (+ seam contract)
│   ├── chunk_streamer.*     grid lifecycle, budgets, retries, door markers
│   ├── floating_origin.*    the whole point: double globals, float locals
│   ├── interior.*           deterministic 3–7 room pockets at z ≥ 1000
│   ├── world_delta.*        what the player changed, nothing else
│   ├── mission.*            JSON missions + runner
│   ├── test_polygon.*       endless road, marker every 200 m (global grid)
│   ├── touch_gesture.*      two-finger swipe detector
│   └── config.*             settings file with clamps
├── src/net/                 CloudSync + JNI HttpURLConnection transport
└── src/ui/                  ImGui overlay (RenderWare backend)
```

## Coordinate model

```
global (double, metres on the US plane)
        ▲  offset (double)
local (float, RenderWare)
```

* `global = offset + local`, always.
* A shift moves every entity by `delta`, then `offset -= delta`; `delta` is always a
  whole number of chunks (negative direction toward zero), so:

  * chunk keys — and therefore seeds, streets and interiors — never change because of
    a shift;
  * after a shift the player stands inside the centre chunk, so the whole active grid
    fits: `max|local| + (radius+1)·200 ≤ 1000`.

With the default 5×5 grid the trigger threshold is `1000 − 600 − 50 = 350 m`; the
unit test `floating_origin_keeps_grid_inside_limit` walks 34 km and asserts the
invariant on every frame.

Interiors are the exception: at `z ≥ 1000` the floating origin is suspended (the
adapter also refuses to shift entities with `m_nInterior != 0`), because an interior
pocket is an independent coordinate space anchored to nothing.

## Determinism

```
ChunkSeed(cx, cy)      = splitmix64(mix(cx) ^ mix(cy))
DoorSeed(cx, cy, slot) = splitmix64(ChunkSeed ^ slot·const)
InteriorSeed(door)     = splitmix64(door ^ const)
```

`Rng` wraps `std::mt19937_64`, whose output is standardised — same door gives the same
rooms, furniture and register on every device. The heightfield is a pure function of
global position, so neighbouring chunks agree on their shared edge by construction
(asserted sample-by-sample in `chunk_edges_match_between_neighbours`).

## Streaming contract

* chunk = 200 × 200 m, grid radius 1 (3×3) or 2 (5×5);
* props are never placed closer than their own footprint to a chunk border, so nothing
  dangles over a boundary (`chunk_props_never_cross_edges`);
* a prop whose model is not resident stays in `pending` and is retried next frame —
  no silent holes;
* unloading destroys objects, removes our markers, and marks models deletable;
* door markers are the entry points of the interior system; the streamer keeps their
  cached local positions in sync across origin shifts.

## Mission data model

```json
{
  "id": "mission_001_robbery",
  "start": {"x": 580347.6, "y": 1063350.9},
  "reward": 2500,
  "steps": [
    {"type": "goto", "target": {"x": 580347.6, "y": 1063350.9}, "radius": 12},
    {"type": "rob_door", "door_seed": 0},
    {"type": "goto", "target": {"x": 580520.0, "y": 1063700.0}, "radius": 10}
  ]
}
```

`door_seed: 0` means "any register", which keeps hand-written missions playable on a
procedural map where you cannot know which shop the player will pick. Progress lives in
`world_delta.json`, so missions survive reloads and CloudSync can replace the JSON
without touching the save.

## CloudSync

`CloudSync` runs a single worker thread, `Poll()` on the game thread applies results:

1. GET `assets/manifest.json`;
2. for each entry compare `version` and FNV-1a 64 `hash` with the local file;
3. download into `<file>.download`, verify the hash, rename atomically;
4. persist `cloud_state.json`, reload missions, toast the player.

Transport is JNI over `java.net.HttpURLConnection` (system trust store, no extra
native dependencies). The host tests exercise the whole state machine with a fake
transport, including hash mismatches and offline manifests.
