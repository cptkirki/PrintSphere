#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${BAMBUSTAT_IDF_IMAGE:-${PRINTSPHERE_IDF_IMAGE:-espressif/idf:release-v6.0}}"
MODE="auto"
TARGET="build"
PULL_IMAGE=0

usage() {
  cat <<'USAGE'
Usage: tools/build_only.sh [--native|--docker] [--pull] [--package]

Build bambustat round AMOLED firmware without flashing or touching serial ports.

Modes:
  --native   Use the host ESP-IDF environment. Requires idf.py on PATH.
  --docker   Use a local Docker image. Default image: espressif/idf:release-v6.0.
  --pull     Allow docker pull before build. Not used by default.
  --package  Also run the release_initial_flash packaging target after build.

Environment:
  BAMBUSTAT_IDF_IMAGE    Override Docker image tag.
  PRINTSPHERE_IDF_IMAGE  Legacy override for the same Docker image tag.

This script never runs idf.py flash, esptool write_flash, or a serial monitor.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --native)
      MODE="native"
      ;;
    --docker)
      MODE="docker"
      ;;
    --pull)
      PULL_IMAGE=1
      ;;
    --package)
      TARGET="release_initial_flash"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

cd "$ROOT"

branch="$(git branch --show-current 2>/dev/null || true)"
if [[ -n "$branch" && "$branch" != "codex/round-amoled-printsphere" ]]; then
  echo "Refusing build: expected branch codex/round-amoled-printsphere, got $branch" >&2
  exit 3
fi

run_native() {
  if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py is not on PATH. Source ESP-IDF export.sh first, or use --docker." >&2
    return 127
  fi
  idf.py reconfigure
  if [[ -x "$ROOT/tools/patches/apply_adapter_patches.sh" ]]; then
    "$ROOT/tools/patches/apply_adapter_patches.sh"
  fi
  idf.py "$TARGET"
}

run_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is not installed or not on PATH." >&2
    return 127
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "Docker daemon is not running." >&2
    return 127
  fi
  if [[ "$PULL_IMAGE" -eq 1 ]]; then
    docker pull "$IMAGE"
  elif ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Missing Docker image: $IMAGE" >&2
    echo "Run again with --pull, or pull it explicitly after accepting the download cost." >&2
    return 127
  fi
  docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e TARGET="$TARGET" \
    -v "$ROOT":/project \
    -w /project \
    "$IMAGE" \
    bash -lc 'idf.py reconfigure && if [[ -x tools/patches/apply_adapter_patches.sh ]]; then tools/patches/apply_adapter_patches.sh; fi && idf.py "$TARGET"'
}

case "$MODE" in
  native)
    run_native
    ;;
  docker)
    run_docker
    ;;
  auto)
    if command -v idf.py >/dev/null 2>&1; then
      run_native
    else
      run_docker
    fi
    ;;
esac
