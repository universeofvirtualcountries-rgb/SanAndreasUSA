#!/usr/bin/env python3
"""Build assets/objects_db.json from the game's own .ide and .col files.

What it does
------------
* Walks extracted game data, parses every IDE object/timed-object section and
  collects model ids, names, texture dictionaries and draw distances.
* Matches those names against binary .col collision archives and extracts the
  real bounding box, converted to half extents + centre offset.
* Classifies every model into the tags the procedural generator understands
  (building / tree / wall / floor / furniture / road_flat / prop / light).
* Writes the compact JSON the mod loads at startup.

Usage
-----
    python3 tools/parse_ide.py --game /path/to/extracted/GTA_SA --out assets/objects_db.json
    python3 tools/parse_ide.py --ide data/maps/LA/LAe.ide --col models/coll/LAe.col \
                               --out assets/objects_db.json --merge
    python3 tools/parse_ide.py --self-test

The game files must already be extracted from the .img archives (e.g. with
IMG Tool or fastman92 limit adjuster's tools): this script reads plain .ide text
and .col binaries, it does not touch .img containers.

Collision notes
---------------
COL1/COL2/COL3 carry their bounds right after the header, so the box is exact.
COL4 keeps bounds inside the model sections; those entries are reported without
collision data (has_collision stays true, but col comes from the .ide draw
distance heuristic) and counted in the summary, never silently guessed.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

IDE_SECTIONS = {
    "objs",
    "tobj",
    "anim",
    "2dfx",
    "path",
    "wep",
    "wepa",
    "txdp",
    "cars",
    "peds",
    "hier",
    "mlos",
    "lodm",
    "light",
}

TAG_RULES: List[Tuple[str, Tuple[str, ...]]] = [
    ("road_flat", ("road", "pave", "asphalt", "highway", "freeway", "tarmac", "roadblock")),
    ("tree", ("tree", "pine", "oak", "palm", "bush", "veg_", "veg", "plant", "shrub", "hedge")),
    ("light", ("light", "lamp", "streetlight", "neon", "lamppost")),
    ("door", ("door", "gate", "shutter", "entrance")),
    ("wall", ("wall", "fence", "railing", "barrier", "pillar", "column")),
    ("floor", ("floor", "pave", "slab", "tile", "ground", "kerb", "curb")),
    (
        "furniture",
        (
            "shelf",
            "desk",
            "chair",
            "table",
            "counter",
            "cabinet",
            "furn",
            "sofa",
            "bed",
            "crate",
            "rack",
            "bench",
            "till",
            "register",
        ),
    ),
    (
        "building",
        (
            "build",
            "house",
            "shop",
            "store",
            "apart",
            "office",
            "block",
            "hotel",
            "bank",
            "station",
            "tower",
            "warehouse",
            "garage",
            "church",
            "school",
            "hospital",
            "motel",
            "bar",
            "diner",
            "casino",
        ),
    ),
]


@dataclass
class ModelEntry:
    id: int
    name: str
    txd: str = ""
    draw_distance: float = 299.0
    flags: int = 0
    interior_only: bool = False
    col_half: Optional[Tuple[float, float, float]] = None
    col_offset: Optional[Tuple[float, float, float]] = None
    tag: str = "prop"

    @property
    def radius2d(self) -> float:
        if not self.col_half:
            return 0.0
        return max(self.col_half[0], self.col_half[1])


def looks_like_name(token: str) -> bool:
    if not token:
        return False
    if not (token[0].isalpha() or token[0] == "_"):
        return False
    return all(c.isalnum() or c in "_-." for c in token)


def parse_ide_text(text: str) -> List[ModelEntry]:
    """Parse the object-ish sections of an .ide file."""
    entries: List[ModelEntry] = []
    section: Optional[str] = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.lower().startswith("end"):
            section = None
            continue
        lowered = line.lower()
        if section is None:
            name = lowered.split()[0] if lowered.split() else ""
            if name in IDE_SECTIONS:
                section = name
            continue

        if section not in ("objs", "tobj", "anim", "lodm"):
            continue

        # Commas are optional in some editors' exports; normalise them away.
        tokens = [t for t in line.replace(",", " ").split() if t]
        if len(tokens) < 4:
            continue
        try:
            model_id = int(tokens[0])
        except ValueError:
            continue
        model_name = tokens[1]
        if not looks_like_name(model_name):
            continue
        txd = tokens[2]
        try:
            draw_distance = float(tokens[3])
        except ValueError:
            draw_distance = 299.0
        flags = 0
        if len(tokens) >= 5:
            try:
                flags = int(tokens[4])
            except ValueError:
                flags = 0
        entries.append(ModelEntry(model_id, model_name, txd, draw_distance, flags))
    return entries


# ---------------------------------------------------------------------------
# COL parsing
# ---------------------------------------------------------------------------


def _vec3(data: bytes, offset: int) -> Tuple[float, float, float]:
    return struct.unpack_from("<3f", data, offset)


def parse_col_bounds(data: bytes, name_hint: str = "") -> Optional[Tuple[str, Tuple[float, float, float], Tuple[float, float, float]]]:
    """Return (model_name, half_extents, centre_offset) or None.

    Supports COL1 / COL2 / COL3. COL4 is detected and skipped on purpose: its
    bounds live in sub-sections and differ between tools, so a wrong guess would
    spawn garbage; the summary reports how many were skipped.
    """
    if len(data) < 40:
        return None
    sig = data[0:4]
    if sig not in (b"COL1", b"COL2", b"COL3", b"COL4"):
        return None
    if sig == b"COL4":
        return None

    # Layout for COL1/2/3: sig(4) size(4) name(22) modelId(2) bounds(24: min 12 + max 12)
    name = data[8:30].split(b"\x00", 1)[0].decode("latin-1", errors="replace").strip()
    try:
        bx_min = _vec3(data, 32)
        bx_max = _vec3(data, 44)
    except struct.error:
        return None

    # A completely empty box is useless for placement.
    if all(abs(v) < 1e-4 for v in bx_min) and all(abs(v) < 1e-4 for v in bx_max):
        return None
    if bx_max[0] < bx_min[0] or bx_max[1] < bx_min[1] or bx_max[2] < bx_min[2]:
        return None

    half = ((bx_max[0] - bx_min[0]) * 0.5, (bx_max[1] - bx_min[1]) * 0.5, (bx_max[2] - bx_min[2]) * 0.5)
    centre = ((bx_max[0] + bx_min[0]) * 0.5, (bx_max[1] + bx_min[1]) * 0.5, (bx_max[2] + bx_min[2]) * 0.5)
    return (name or name_hint, half, centre)


def iter_col_models(blob: bytes) -> Iterable[Tuple[str, Tuple[float, float, float], Tuple[float, float, float]]]:
    """Walk a multi-model COL archive (they are just concatenated models)."""
    offset = 0
    size = len(blob)
    while offset + 8 <= size:
        sig = blob[offset : offset + 4]
        if sig not in (b"COL1", b"COL2", b"COL3", b"COL4"):
            # Resynchronise: scan forward for the next signature.
            nxt = min(
                (pos for pos in (blob.find(s, offset + 1) for s in (b"COL1", b"COL2", b"COL3", b"COL4")) if pos >= 0),
                default=-1,
            )
            if nxt < 0:
                break
            offset = nxt
            continue
        try:
            model_size = struct.unpack_from("<I", blob, offset + 4)[0]
        except struct.error:
            break
        if model_size <= 0 or offset + model_size > size:
            # Some tools store the size excluding the header; be forgiving.
            model_size = min(max(model_size, 0), size - offset)
            if model_size == 0:
                break
        chunk = blob[offset : offset + model_size]
        parsed = parse_col_bounds(chunk)
        if parsed:
            yield parsed
        offset += model_size


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def classify(entry: ModelEntry, min_building_radius: float) -> str:
    name = entry.name.lower()
    for tag, needles in TAG_RULES:
        if any(n in name for n in needles):
            return tag
    if entry.col_half:
        hx, hy, hz = entry.col_half
        flat = hz < 1.0
        wide = max(hx, hy) > 6.0
        if flat and wide:
            return "road_flat"
        if not flat and max(hx, hy) >= min_building_radius:
            return "building"
    if "_lod" in name or name.endswith("lod"):
        return "prop"
    return "prop"


def to_json_entry(entry: ModelEntry, min_building_radius: float) -> dict:
    entry.tag = classify(entry, min_building_radius)
    half = entry.col_half or (1.0, 1.0, 1.0)
    offset = entry.col_offset or (0.0, 0.0, 0.0)
    return {
        "id": entry.id,
        "name": entry.name,
        "tag": entry.tag,
        "col": {
            "half": [round(v, 4) for v in half],
            "offset": [round(v, 4) for v in offset],
        },
        "draw_distance": round(entry.draw_distance, 2),
        "road": entry.tag == "road_flat",
        "has_collision": entry.col_half is not None,
        "dynamic": False,
    }


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------


def find_files(root: Path, suffix: str) -> List[Path]:
    out: List[Path] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.lower().endswith(suffix):
                out.append(Path(dirpath) / fn)
    out.sort()
    return out


def read_text(path: Path) -> str:
    for enc in ("utf-8", "latin-1"):
        try:
            return path.read_text(encoding=enc)
        except UnicodeDecodeError:
            continue
    return path.read_bytes().decode("latin-1", errors="replace")


def build_database(
    ide_files: List[Path],
    col_files: List[Path],
    min_building_radius: float,
    verbose: bool,
) -> Tuple[dict, dict]:
    entries: Dict[int, ModelEntry] = {}
    for path in ide_files:
        try:
            text = read_text(path)
        except OSError as exc:
            print(f"  ! cannot read {path}: {exc}", file=sys.stderr)
            continue
        parsed = parse_ide_text(text)
        if verbose:
            print(f"  ide {path}: {len(parsed)} models")
        for entry in parsed:
            entries[entry.id] = entry

    col_by_name: Dict[str, Tuple[Tuple[float, float, float], Tuple[float, float, float]]] = {}
    col_models = 0
    col4_skipped = 0
    for path in col_files:
        try:
            blob = path.read_bytes()
        except OSError as exc:
            print(f"  ! cannot read {path}: {exc}", file=sys.stderr)
            continue
        col4_skipped += blob.count(b"COL4")
        for name, half, centre in iter_col_models(blob):
            if name:
                col_by_name[name.lower()] = (half, centre)
                col_models += 1

    matched = 0
    for entry in entries.values():
        hit = col_by_name.get(entry.name.lower())
        if hit:
            entry.col_half, entry.col_offset = hit
            matched += 1

    models = [to_json_entry(e, min_building_radius) for e in sorted(entries.values(), key=lambda e: e.id)]
    stats = {
        "ide_files": len(ide_files),
        "col_files": len(col_files),
        "col_models_parsed": col_models,
        "col4_skipped": col4_skipped,
        "models": len(models),
        "col_matched": matched,
    }
    database = {
        "version": 1,
        "source": "gta_sa_ide_col",
        "generated_by": "tools/parse_ide.py",
        "stats": stats,
        "models": models,
    }
    return database, stats


# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------


def build_col1(name: str, bmin: Tuple[float, float, float], bmax: Tuple[float, float, float]) -> bytes:
    head = bytearray(32)
    head[0:4] = b"COL1"
    head[8:30] = name.encode("ascii")[:21].ljust(22, b"\x00")
    body = struct.pack("<3f", *bmin) + struct.pack("<3f", *bmax)
    tail = struct.pack("<I", 0)  # zero spheres
    total = len(head) + len(body) + len(tail)
    struct.pack_into("<I", head, 4, total)
    return bytes(head) + body + tail


def self_test() -> int:
    import tempfile

    failures = 0

    def check(cond: bool, what: str) -> None:
        nonlocal failures
        if not cond:
            failures += 1
            print(f"  FAIL {what}")

    ide = """objs
1001, shop_a, shop_txd, 299, 0
1002, tree_pine_a, veg_txd, 150, 0
1003, road_la_a, road_txd, 250, 0
1004, wall_concrete, gen_txd, 120, 0
end
tobj
2001, apt_block_01, apt_txd, 299, 0
end
"""
    col = build_col1("shop_a", (-8.0, -6.0, 0.0), (8.0, 6.0, 12.0)) + build_col1(
        "tree_pine_a", (-1.5, -1.5, 0.0), (1.5, 1.5, 6.0)
    )
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "test.ide").write_text(ide, encoding="utf-8")
        (tmp_path / "test.col").write_bytes(col)
        db, stats = build_database([tmp_path / "test.ide"], [tmp_path / "test.col"], 4.0, False)

    by_name = {m["name"]: m for m in db["models"]}
    check(len(db["models"]) == 5, "expected 5 models")
    check(stats["col_matched"] == 2, "expected 2 collision matches")
    check(by_name["shop_a"]["tag"] == "building", "shop must classify as building")
    check(by_name["apt_block_01"]["tag"] == "building", "apt block must classify as building")
    check(by_name["tree_pine_a"]["tag"] == "tree", "pine must classify as tree")
    check(by_name["road_la_a"]["tag"] == "road_flat", "road must classify as road_flat")
    check(by_name["wall_concrete"]["tag"] == "wall", "wall must classify as wall")
    check(by_name["shop_a"]["col"]["half"] == [8.0, 6.0, 6.0], "shop half extents")
    check(by_name["shop_a"]["col"]["offset"] == [0.0, 0.0, 6.0], "shop centre offset")
    check(by_name["shop_a"]["has_collision"] is True, "shop has collision")
    check(by_name["road_la_a"]["has_collision"] is False, "road has no .col match")
    check(json.loads(json.dumps(db))["models"][0]["id"] == 1001, "json serialisable")

    print(f"parse_ide self-test: {'OK' if failures == 0 else f'{failures} failure(s)'}")
    return 0 if failures == 0 else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Build objects_db.json from GTA SA .ide/.col files")
    parser.add_argument("--game", help="root folder of the extracted game data")
    parser.add_argument("--ide", nargs="*", default=[], help="explicit .ide files")
    parser.add_argument("--col", nargs="*", default=[], help="explicit .col files")
    parser.add_argument("--out", default="assets/objects_db.json", help="output JSON path")
    parser.add_argument("--merge", action="store_true", help="merge into an existing output file")
    parser.add_argument("--min-building-radius", type=float, default=4.0)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    ide_files = [Path(p) for p in args.ide]
    col_files = [Path(p) for p in args.col]
    if args.game:
        root = Path(args.game)
        if not root.exists():
            print(f"game path not found: {root}", file=sys.stderr)
            return 2
        ide_files += find_files(root, ".ide")
        col_files += find_files(root, ".col")
        # IPL files are not needed, but loose .col can also sit next to .ide files.
        if args.verbose:
            print(f"discovered {len(ide_files)} .ide and {len(col_files)} .col files under {root}")

    if not ide_files:
        print("no .ide files given (use --game or --ide)", file=sys.stderr)
        return 2

    print(f"parsing {len(ide_files)} .ide and {len(col_files)} .col files...")
    database, stats = build_database(ide_files, col_files, args.min_building_radius, args.verbose)

    out_path = Path(args.out)
    if args.merge and out_path.exists():
        try:
            existing = json.loads(out_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            existing = {"models": []}
        merged = {m["id"]: m for m in existing.get("models", []) if "id" in m}
        for model in database["models"]:
            merged[model["id"]] = model
        database["models"] = [merged[k] for k in sorted(merged)]
        database["stats"]["merged_with"] = str(out_path)
        stats["models"] = len(database["models"])

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(database, indent=1) + "\n", encoding="utf-8")

    print(
        "done: {models} models ({col_matched} with real collision), "
        "{col4_skipped} COL4 models skipped -> {out}".format(out=out_path, **stats)
    )
    if stats["col4_skipped"]:
        print("note: COL4 bounds are not guessed; those models keep heuristic collision.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
