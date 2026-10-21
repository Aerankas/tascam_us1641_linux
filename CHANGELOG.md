# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Added fallback preprocessor macros for `PW_KEY_NODE_PHYSICAL`, `PW_KEY_NODE_TERMINAL`, and related PipeWire properties, fixing compilation failure on older `libpipewire-0.3` versions such as PipeWire < 1.2 on Ubuntu 24.04 LTS and Linux Mint 22 ([#1](https://github.com/Aerankas/tascam_us1641_linux/issues/1)).
- Added self-healing recovery for USB Bulk IN endpoint stalls (`EP 0x86`) via `libusb_clear_halt()`, preventing permanent audio capture silence on host idle or USB power management transitions ([#3](https://github.com/Aerankas/tascam_us1641_linux/issues/3)).

## [1.0.0] - 2026-10-16

### Added
- Initial public release of `tascamd` user-space driver daemon and TASCAM US-1641 Control Panel GUI.
- Full-duplex PipeWire audio bridge for TASCAM US-1641 (USB ID `0644:8017`):
  - 16-channel capture over Bulk IN EP `0x86` with hardware 24-bit bitplane deinterleaving across 14 analog inputs (Mics 1–8, Line 9–14) and 2 S/PDIF digital inputs.
  - 4-channel playback over Isochronous OUT EP `0x02` (Main Outputs / Headphones + Line Outputs 3/4).
  - Isochronous IN EP `0x81` feedback handling for hardware DAC clock synchronization.
  - Lock-free ring buffer architecture with real-time thread safety.
- Graphical desktop control panel written in PyQt6:
  - Sample rate selection (48,000 Hz / 44,100 Hz).
  - Latency quantum and jitter buffer pre-roll adjustment.
  - Live hardware telemetry, stream health monitoring, and xrun tracking.
  - Desktop entry and application menu integration.
- Automated installation and setup script (`install.sh`) supporting APT, DNF, and Pacman package managers.
- Systemd user service integration (`systemd/tascamd.service.in`) with auto-start on login.
- Udev permissions rule (`udev/90-tascam-us1641.rules`) for non-root hardware access.
- Complete hardware protocol documentation and reverse-engineering specifications (`docs/protocol-spec.md`).

