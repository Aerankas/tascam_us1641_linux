#!/usr/bin/env bash
# install.sh - Automated installer for TASCAM US-1641 Linux PipeWire Driver & Control Panel
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

C_RESET='\033[0m'
C_GREEN='\033[0;32m'
C_BLUE='\033[0;34m'
C_YELLOW='\033[0;33m'
C_RED='\033[0;31m'

info()    { printf "${C_BLUE}[INFO]${C_RESET} %s\n" "$*"; }
success() { printf "${C_GREEN}[OK]${C_RESET} %s\n" "$*"; }
warn()    { printf "${C_YELLOW}[WARN]${C_RESET} %s\n" "$*"; }
error()   { printf "${C_RED}[ERROR]${C_RESET} %s\n" "$*" >&2; }

echo "================================================================"
echo "  TASCAM US-1641 Linux PipeWire Driver & Control Panel Installer"
echo "================================================================"

# 1. Check build & runtime dependencies
check_dependencies() {
    if pkg-config --exists libpipewire-0.3 libusb-1.0 2>/dev/null && command -v gcc >/dev/null 2>&1; then
        success "Build dependencies (gcc, libpipewire-0.3, libusb-1.0) found."
        return 0
    fi

    warn "Missing build or runtime dependencies. Attempting to install via system package manager..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y build-essential pkg-config libpipewire-0.3-dev libusb-1.0-0-dev python3-pyqt6 qpwgraph
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y gcc make pkgconf-pkg-config pipewire-devel libusb1-devel python3-pyqt6 qpwgraph
    elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -S --needed base-devel pkgconf libpipewire libusb python-pyqt6 qpwgraph
    else
        error "Could not detect package manager (apt, dnf, pacman)."
        error "Please install gcc, pkg-config, libpipewire-0.3-dev, libusb-1.0-0-dev, and python3-pyqt6 manually."
        exit 1
    fi
}

check_dependencies

# 2. Configure udev rule if not already present
UDEV_FILE="/etc/udev/rules.d/90-tascam-us1641.rules"
if [[ -f "$UDEV_FILE" ]] && grep -q "8017" "$UDEV_FILE"; then
    success "udev permission rule already present at $UDEV_FILE"
else
    info "Installing USB permission rule to $UDEV_FILE (requires sudo)..."
    make install-udev
fi

# 3. Build and install user-space binary, systemd service, and GUI
info "Building and installing tascamd & TASCAM Control Panel..."
make install

# 4. Create default ~/.config/tascamd.conf if not present
CONF_FILE="${HOME}/.config/tascamd.conf"
if [[ ! -f "$CONF_FILE" ]]; then
    mkdir -p "${HOME}/.config"
    cat << 'EOF' > "$CONF_FILE"
# TASCAM US-1641 Hardware Bridge Configuration
sample_rate = 48000
preroll_frames = 2048
EOF
    success "Created default configuration at $CONF_FILE (48,000 Hz)"
fi

# 5. Enable and restart systemd service
info "Enabling and starting tascamd.service..."
systemctl --user enable tascamd.service
systemctl --user restart tascamd.service

echo ""
echo "================================================================"
success "Installation Complete!"
echo "  • Daemon Status : systemctl --user status tascamd"
echo "  • Control Panel : Search 'TASCAM' in your application menu,"
echo "                    or run: ~/.local/bin/tascam-control-panel"
echo "================================================================"
