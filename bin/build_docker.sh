#!/usr/bin/env bash
# Build the Freedea firmware inside a container (docker or podman) and place
# the binary in dist/freedea-x4-<version>.bin. No local toolchain required.
#
# Windows: use bin/build_docker.ps1 (same CLI).
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PIO_ENV="x4_release"
NO_CACHE=""
TOOL="${CONTAINER_TOOL:-}"

usage() {
  cat <<'EOF'
Usage: bin/build_docker.sh [--env <pio-env>] [--tool docker|podman] [--no-cache]

  --env <pio-env>   PlatformIO environment (default: x4_release)
  --tool <name>     Container tool (default: $CONTAINER_TOOL, else docker,
                    else podman)
  --no-cache        Build the image without layer cache

Requires: git, docker OR podman. Submodules must be initialized.
Output:   dist/freedea-x4-<version>.bin (or freedea-<env>-<version>.bin for
          non-default envs), version from src/AppVersion.h.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --env) PIO_ENV="${2:?--env needs a value}"; shift 2 ;;
    --tool) TOOL="${2:?--tool needs a value}"; shift 2 ;;
    --no-cache) NO_CACHE="--no-cache"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ -z "$TOOL" ]; then
  if command -v docker >/dev/null 2>&1; then TOOL=docker
  elif command -v podman >/dev/null 2>&1; then TOOL=podman
  else echo "No container tool found (install docker or podman, or set CONTAINER_TOOL)." >&2; exit 1
  fi
fi
command -v "$TOOL" >/dev/null 2>&1 || { echo "Container tool '$TOOL' not found." >&2; exit 1; }

# Submodule preflight: an uninitialized entry shows a leading '-' in the status.
if git -C "$ROOT" submodule status --recursive 2>/dev/null | grep -q '^- '; then
  echo "Submodules are not initialized. Run: git submodule update --init --recursive" >&2
  exit 1
fi

VERSION="$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' "$ROOT/src/AppVersion.h")"
[ -n "$VERSION" ] || { echo "Could not parse kAppVersion from src/AppVersion.h" >&2; exit 1; }

if [ "$PIO_ENV" = "x4_release" ]; then
  OUT="$ROOT/dist/freedea-x4-$VERSION.bin"
else
  OUT="$ROOT/dist/freedea-$PIO_ENV-$VERSION.bin"
fi
mkdir -p "$ROOT/dist"

# Single untagged build straight to the artifact-carrier (export) stage — the
# build stages run as its dependencies. The image exists only to hand out
# /freedea.bin: --iidfile captures its ID (docker+podman), and after the copy
# both container and image are deleted; only the build layers stay in the
# local cache. --platform: Apple-Silicon hosts build the amd64 image
# transparently (the toolchain is amd64-native inside).
IID_FILE="$(mktemp)"
# shellcheck disable=SC2086  # NO_CACHE is an optional single word
"$TOOL" build $NO_CACHE --platform linux/amd64 -f "$ROOT/docker/Dockerfile" \
  --target export --iidfile "$IID_FILE" --build-arg "PIO_ENV=$PIO_ENV" "$ROOT"
IID="$(cat "$IID_FILE")"
rm -f "$IID_FILE"
[ -n "$IID" ] || { echo "Build produced no image ID." >&2; exit 1; }

CID="$("$TOOL" create "$IID")"
trap '{ "$TOOL" rm "$CID" >/dev/null 2>&1 || true; "$TOOL" rmi "$IID" >/dev/null 2>&1 || true; }' EXIT
"$TOOL" cp "$CID:/freedea.bin" "$OUT"
echo "Wrote $OUT"
if command -v sha256sum >/dev/null 2>&1; then sha256sum "$OUT"
elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$OUT"; fi
