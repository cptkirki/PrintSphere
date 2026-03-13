#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Iterable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Merge ESP-IDF flash artifacts into one initial-flash firmware.bin."
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="ESP-IDF build directory containing flasher_args.json",
    )
    parser.add_argument(
        "--output",
        default="release/firmware.bin",
        help="Output path for the merged firmware image",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Optional release version suffix such as v1",
    )
    return parser.parse_args()


def load_flash_entries(build_dir: Path) -> list[tuple[int, Path]]:
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise FileNotFoundError(f"Missing {flasher_args_path}")

    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    flash_files = flasher_args.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json does not contain flash_files")

    entries: list[tuple[int, Path]] = []
    for offset_text, relative_path in flash_files.items():
        offset = int(offset_text, 0)
        image_path = build_dir / relative_path
        if not image_path.is_file():
            raise FileNotFoundError(f"Missing flash image {image_path}")
        entries.append((offset, image_path))

    return sorted(entries, key=lambda item: item[0])


def validate_no_conflicts(image: bytearray, offset: int, payload: bytes) -> None:
    end = offset + len(payload)
    for index, value in enumerate(payload, start=offset):
        existing = image[index]
        if existing != 0xFF and existing != value:
            conflict_at = hex(index)
            raise ValueError(f"Overlapping flash segments conflict at {conflict_at}")
    image[offset:end] = payload


def merge_flash_entries(entries: Iterable[tuple[int, Path]]) -> bytearray:
    materialized = [(offset, path, path.read_bytes()) for offset, path in entries]
    if not materialized:
        raise ValueError("No flash entries to merge")

    image_size = max(offset + len(payload) for offset, _, payload in materialized)
    merged = bytearray(b"\xFF" * image_size)
    for offset, _, payload in materialized:
        validate_no_conflicts(merged, offset, payload)
    return merged


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    output_path = Path(args.output).resolve()
    release_version = args.version.strip()

    entries = load_flash_entries(build_dir)
    merged = merge_flash_entries(entries)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(merged)

    versioned_output_path: Path | None = None
    if release_version:
        versioned_output_path = output_path.with_name(
            f"{output_path.stem}-{release_version}{output_path.suffix}"
        )
        versioned_output_path.write_bytes(merged)

    digest = hashlib.sha256(merged).hexdigest()
    print(f"Wrote {output_path}")
    if versioned_output_path is not None:
        print(f"Wrote {versioned_output_path}")
    print(f"Size: {len(merged)} bytes")
    print(f"SHA256: {digest}")
    for offset, image_path in entries:
        print(f"  {offset:#08x}  {image_path.relative_to(build_dir)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
