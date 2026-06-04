#!/usr/bin/env bash
# Initialize submodule and apply zenoh-pico patches required for the ESP-NOW transport.
# Run once after cloning: bash scripts/setup.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZENOH_PICO_DIR="$REPO_ROOT/zenoh-pico"
PATCH_DIR="$REPO_ROOT/patches"

echo "Initializing submodule..."
git -C "$REPO_ROOT" submodule update --init

if [ ! -d "$ZENOH_PICO_DIR/.git" ] && [ ! -f "$ZENOH_PICO_DIR/.git" ]; then
    echo "ERROR: zenoh-pico submodule not found after init."
    exit 1
fi

cd "$ZENOH_PICO_DIR"

echo "Applying patches..."
for patch in "$PATCH_DIR"/*.patch; do
    echo "  $patch ..."
    git apply --check "$patch" 2>/dev/null && git apply "$patch" || echo "  (already applied, skipping)"
done

echo "Done."
