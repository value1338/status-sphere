#!/usr/bin/env python3
"""Convert layout image assets to LVGL RGB565A8 C sources for ESP32 firmware."""

from __future__ import annotations

import argparse
import base64
import io
import json
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    sys.exit(1)

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
ASSETS_DIR = PROJECT_ROOT / "main" / "resources" / "assets"
OUT_CPP = PROJECT_ROOT / "main" / "src" / "layout_assets_registry.cpp"
OUT_HPP = PROJECT_ROOT / "main" / "include" / "printsphere" / "layout_assets_registry.hpp"

MAX_DIMENSION = 512
SUPPORTED_EXT = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"}


def c_ident(asset_id: str) -> str:
    safe = re.sub(r"[^a-zA-Z0-9_]", "_", asset_id)
    if not safe or safe[0].isdigit():
        safe = f"a_{safe}"
    return f"layout_asset_{safe}"


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def image_to_rgb565a8(img: Image.Image) -> tuple[int, int, bytes]:
    rgba = img.convert("RGBA")
    w, h = rgba.size
    if w > MAX_DIMENSION or h > MAX_DIMENSION:
        raise ValueError(f"Image too large ({w}x{h}), max {MAX_DIMENSION}px per side")
    px = rgba.load()
    out = bytearray(w * h * 3)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            c = rgb565(r, g, b)
            out[i] = c & 0xFF
            out[i + 1] = (c >> 8) & 0xFF
            out[i + 2] = a
            i += 3
    return w, h, bytes(out)


def format_c_array(data: bytes, indent: str = "  ") -> str:
    lines: list[str] = []
    row: list[str] = []
    for i, b in enumerate(data):
        row.append(f"0x{b:02x}")
        if len(row) == 16:
            lines.append(indent + ", ".join(row) + ",")
            row = []
    if row:
        lines.append(indent + ", ".join(row) + ",")
    return "\n".join(lines)


def load_image_bytes(path: Path) -> bytes:
    return path.read_bytes()


def load_image_from_b64(data_b64: str, mime: str = "") -> Image.Image:
    raw = base64.b64decode(data_b64)
    img = Image.open(io.BytesIO(raw))
    return img


def collect_asset_ids(layout: dict) -> set[str]:
    ids: set[str] = set()
    assets = layout.get("assets") or {}
    if isinstance(assets, dict):
        ids.update(assets.keys())
    for page in layout.get("pages") or []:
        for el in page.get("elements") or []:
            if el.get("type") == "image" and el.get("asset"):
                ids.add(el["asset"])
    return ids


def write_stub() -> None:
    OUT_HPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_HPP.write_text(
        """#pragma once

#include "lvgl.h"

namespace printsphere {

const lv_image_dsc_t* layout_asset_lookup(const char* asset_id);

}  // namespace printsphere
""",
        encoding="utf-8",
    )
    OUT_CPP.write_text(
        """#include "printsphere/layout_assets_registry.hpp"

namespace printsphere {

const lv_image_dsc_t* layout_asset_lookup(const char* asset_id) {
  (void)asset_id;
  return nullptr;
}

}  // namespace printsphere
""",
        encoding="utf-8",
    )


def convert_assets(layout: dict | None = None, layout_path: Path | None = None) -> int:
    if layout is None and layout_path is not None:
        layout = json.loads(layout_path.read_text(encoding="utf-8"))
    if layout is None:
        write_stub()
        return 0

    asset_ids = sorted(collect_asset_ids(layout))
    assets_meta = layout.get("assets") or {}

    if not asset_ids:
        write_stub()
        return 0

    ASSETS_DIR.mkdir(parents=True, exist_ok=True)
    entries: list[tuple[str, str, int, int, str]] = []

    for asset_id in asset_ids:
        c_name = c_ident(asset_id)
        meta = assets_meta.get(asset_id) if isinstance(assets_meta, dict) else None
        img: Image.Image | None = None

        png_path = ASSETS_DIR / f"{asset_id}.png"
        for ext in SUPPORTED_EXT:
            p = ASSETS_DIR / f"{asset_id}{ext}"
            if p.is_file():
                img = Image.open(p)
                break

        if img is None and isinstance(meta, dict) and meta.get("data"):
            img = load_image_from_b64(meta["data"], meta.get("mime", ""))
            png_path.write_bytes(base64.b64decode(meta["data"]))

        if img is None:
            print(f"Warning: missing asset '{asset_id}', skipping", file=sys.stderr)
            continue

        w, h, raw = image_to_rgb565a8(img)
        entries.append((asset_id, c_name, w, h, format_c_array(raw)))

    if not entries:
        write_stub()
        return 0

    cpp_parts = [
        '#include "printsphere/layout_assets_registry.hpp"',
        "",
        "#include <cstring>",
        "",
        "namespace printsphere {",
        "",
    ]
    lookup_cases: list[str] = []

    for asset_id, c_name, w, h, arr in entries:
        map_name = f"{c_name}_map"
        dsc_name = c_name
        cpp_parts.extend(
            [
                f"static const uint8_t {map_name}[] = {{",
                arr,
                "};",
                "",
                f"static const lv_image_dsc_t {dsc_name} = {{",
                "  .header.cf = LV_COLOR_FORMAT_RGB565A8,",
                "  .header.magic = LV_IMAGE_HEADER_MAGIC,",
                f"  .header.w = {w},",
                f"  .header.h = {h},",
                f"  .data_size = {w * h * 3},",
                f"  .data = {map_name},",
                "};",
                "",
            ]
        )
        lookup_cases.append(f'  if (asset_id != nullptr && std::strcmp(asset_id, "{asset_id}") == 0) {{')
        lookup_cases.append(f"    return &{dsc_name};")
        lookup_cases.append("  }")

    cpp_parts.extend(
        [
            "const lv_image_dsc_t* layout_asset_lookup(const char* asset_id) {",
            *lookup_cases,
            "  return nullptr;",
            "}",
            "",
            "}  // namespace printsphere",
            "",
        ]
    )

    OUT_HPP.write_text(
        """#pragma once

#include "lvgl.h"

namespace printsphere {

const lv_image_dsc_t* layout_asset_lookup(const char* asset_id);

}  // namespace printsphere
""",
        encoding="utf-8",
    )
    OUT_CPP.write_text("\n".join(cpp_parts), encoding="utf-8")
    print(f"Converted {len(entries)} asset(s) -> {OUT_CPP}")
    return len(entries)


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert layout image assets to LVGL C arrays")
    parser.add_argument("--layout", type=Path, help="Path to layout.json")
    args = parser.parse_args()

    layout_path = args.layout or (PROJECT_ROOT / "main" / "resources" / "layout.json")
    if layout_path.is_file():
        convert_assets(layout_path=layout_path)
    else:
        write_stub()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
