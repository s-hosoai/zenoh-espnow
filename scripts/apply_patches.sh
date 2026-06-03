#!/usr/bin/env bash
# Apply zenoh-pico patches required for the ESP-NOW transport override.
# Run this once after `git submodule update --init`.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZENOH_PICO_DIR="$REPO_ROOT/third_party/zenoh-pico"
PATCH_DIR="$REPO_ROOT/components/zenoh_espnow/zenoh_espnow_patch"

if [ ! -d "$ZENOH_PICO_DIR/.git" ]; then
    echo "ERROR: zenoh-pico submodule not found. Run: git submodule update --init"
    exit 1
fi

cd "$ZENOH_PICO_DIR"

for patch in "$PATCH_DIR"/*.patch; do
    echo "Applying $patch ..."
    git apply --check "$patch" 2>/dev/null && git apply "$patch" || echo "  (already applied, skipping)"
done

echo "Done."
