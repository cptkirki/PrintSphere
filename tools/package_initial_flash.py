#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from typing import Iterable


def app_identity(data: bytes) -> dict:
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("Missing ESP32 application descriptor")
    def field(offset: int) -> str:
        return data[offset:offset + 32].split(b"\0", 1)[0].decode("ascii")
    result = {"version": field(48), "project": field(80), "idf": field(144)}
    boards = {"printsphere_2_8c_p2": "lcd_2_8c", "printsphere_1_75_p2": "amoled_1_75"}
    if result["project"] not in boards:
        raise ValueError("Image is not a board-identified PrintSphere application")
    result["board"] = boards[result["project"]]
    return result


def source_tree_hash(source_root: Path) -> str:
    files = {p for directory in ("main", "components", "tools/patches")
             for p in (source_root / directory).rglob("*")
             if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc"}
    files.update(source_root / name for name in (
        "VERSION", "CMakeLists.txt", "dependencies.lock", "sdkconfig.defaults", "partitions.csv",
        "tools/package_initial_flash.py"))
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda p: p.relative_to(source_root).as_posix()):
        digest.update(path.relative_to(source_root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Merge ESP-IDF flash artifacts into one initial-flash firmware.bin."
    )
    parser.add_argument(
        "--release-root",
        default="release",
        help="Release root directory. Example: release or release/2.8c",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="ESP-IDF build directory containing flasher_args.json",
    )
    parser.add_argument(
        "--output",
        default="release/initial/printsphere_full.bin",
        help="Output path for the merged firmware image",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Optional release version suffix such as v1.4.1",
    )
    parser.add_argument(
        "--ota-output",
        default="release/ota/printsphere_ota.bin",
        help="Output path for the OTA-flashable app-only firmware image",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Package as debug build: output goes to release/debug/, versioned copies to release/debug/archive/",
    )
    parser.add_argument(
        "--beta",
        action="store_true",
        help="Package as beta build: output goes to release/beta/, versioned copies to release/beta/archive/",
    )
    return parser.parse_args()


def load_app_binary(build_dir: Path) -> tuple[Path, bytes]:
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise FileNotFoundError(f"Missing {flasher_args_path}")

    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    app_entry = flasher_args.get("app")
    if not isinstance(app_entry, dict) or not app_entry.get("file"):
        raise ValueError("flasher_args.json does not contain an 'app' entry with a file path")

    app_path = build_dir / app_entry["file"]
    if not app_path.is_file():
        raise FileNotFoundError(f"Missing app binary {app_path}")

    return app_path, app_path.read_bytes()


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
    release_version = args.version.strip()
    release_root = Path(args.release_root)

    # Safety check: a version containing "_debug" must use --debug so debug
    # builds never land in the stable release/initial/ or release/ota/ folders.
    if "_debug" in release_version and not args.debug:
        print(
            f"ERROR: version '{release_version}' contains '_debug' but --debug flag is not set.\n"
            "       Add --debug to package into release/beta/, or use a non-debug version string.",
            flush=True,
        )
        return 1

    if args.debug and args.beta:
        print("ERROR: --debug and --beta are mutually exclusive.", flush=True)
        return 1

    # Resolve output paths: --debug and --beta both go to release/beta/
    if args.debug or args.beta:
        output_path = (release_root / "beta" / "printsphere_full.bin").resolve()
        ota_output_path = (release_root / "beta" / "printsphere_ota.bin").resolve()
    else:
        default_output = Path("release/initial/printsphere_full.bin")
        default_ota_output = Path("release/ota/printsphere_ota.bin")

        output_arg = Path(args.output)
        ota_output_arg = Path(args.ota_output)

        if output_arg == default_output:
            output_path = (release_root / "initial" / "printsphere_full.bin").resolve()
        else:
            output_path = output_arg.resolve()

        if ota_output_arg == default_ota_output:
            ota_output_path = (release_root / "ota" / "printsphere_ota.bin").resolve()
        else:
            ota_output_path = ota_output_arg.resolve()

    # Validate the actual image before writing or replacing any release artifact.
    app_path, app_bytes = load_app_binary(build_dir)
    identity = app_identity(app_bytes)
    expected_version = identity["version"] + ("-2.8c" if identity["board"] == "lcd_2_8c" else "")
    if release_version and release_version != expected_version:
        raise ValueError(f"Package version {release_version!r} differs from image {expected_version!r}")
    release_version = expected_version
    entries = load_flash_entries(build_dir)
    merged = merge_flash_entries(entries)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(merged)

    versioned_output_path: Path | None = None
    if release_version:
        archive_dir = output_path.parent / "archive"
        archive_dir.mkdir(parents=True, exist_ok=True)
        versioned_output_path = archive_dir / f"{output_path.stem}-{release_version}{output_path.suffix}"
        versioned_output_path.write_bytes(merged)

    digest = hashlib.sha256(merged).hexdigest()
    print(f"Wrote {output_path}")
    if versioned_output_path is not None:
        print(f"Wrote {versioned_output_path}")
    print(f"Size: {len(merged)} bytes")
    print(f"SHA256: {digest}")
    for offset, image_path in entries:
        print(f"  {offset:#08x}  {image_path.relative_to(build_dir)}")

    # --- OTA-flashable app binary ---
    app_path, app_bytes = load_app_binary(build_dir)
    ota_output_path.parent.mkdir(parents=True, exist_ok=True)
    ota_output_path.write_bytes(app_bytes)

    ota_versioned_path: Path | None = None
    if release_version:
        ota_archive_dir = ota_output_path.parent / "archive"
        ota_archive_dir.mkdir(parents=True, exist_ok=True)
        ota_versioned_path = ota_archive_dir / f"{ota_output_path.stem}-{release_version}{ota_output_path.suffix}"
        ota_versioned_path.write_bytes(app_bytes)

    ota_digest = hashlib.sha256(app_bytes).hexdigest()
    print(f"\nWrote OTA binary {ota_output_path}")
    if ota_versioned_path is not None:
        print(f"Wrote OTA binary {ota_versioned_path}")
    print(f"OTA size:   {len(app_bytes)} bytes  (source: {app_path.name})")
    print(f"OTA SHA256: {ota_digest}")

    source_root = Path(__file__).resolve().parents[1]
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=source_root,
                              capture_output=True, text=True, check=True).stdout.strip()
    manifest = {
        **identity, "idf_required": "5.5.4", "package_version": release_version, "git_commit": revision,
        "source_tree_sha256": source_tree_hash(source_root),
        "dependencies_lock_sha256": hashlib.sha256((source_root / "dependencies.lock").read_bytes()).hexdigest(),
        "sdkconfig_sha256": hashlib.sha256((build_dir / "config/sdkconfig.h").read_bytes()).hexdigest(),
        "ota": {"bytes": len(app_bytes), "sha256": ota_digest},
        "initial": {"bytes": len(merged), "sha256": digest},
    }
    (output_path.parent.parent / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
