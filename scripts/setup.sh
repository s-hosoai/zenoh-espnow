#!/usr/bin/env bash
# Initialize the zenoh-pico submodule.
# Run once after cloning: bash scripts/setup.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "Initializing submodule..."
git -C "$REPO_ROOT" submodule update --init

echo "Done."
