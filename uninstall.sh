#!/usr/bin/env bash
# uninstall.sh - Clean uninstaller for TASCAM US-1641 Linux Driver
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

make uninstall
