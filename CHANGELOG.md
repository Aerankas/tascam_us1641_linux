# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-10-29

### Added
- Native 32-bit floating-point PipeWire stream bridging (`SPA_AUDIO_FORMAT_F32_LE`) and 24-bit packed PCM on `EP 0x02`, eliminating 16-bit truncation and quantization distortion ([#10](https://github.com/Aerankas/tascam_us1641_linux/issues/10)).
- Direct hardware feedback pacing (`EP 0x81` $\to$ `EP 0x02`) utilizing a lockless token FIFO to synchronize USB transfers with the physical DAC consumption rate matching the official Windows driver.
- Configured native Pro Audio channel positions (`audio.position = [ AUX0..AUX15 ]`) and disabled sound server channel matrixing (`channelmix.disable = true`) on `tascam_us1641_source`.
- Full hardware MIDI Input and Output support for the US-1641's 5-pin DIN ports via USB bulk endpoints `EP 0x83` (MIDI IN) and `EP 0x04` (MIDI OUT) ([#7](https://github.com/Aerankas/tascam_us1641_linux/issues/7)):
  - Implemented 9-byte USB frame packetizer and depacketizer with `0xFD` pad filtering and `0xE0` delimiter alignment.
  - Native ALSA Sequencer integration (`TASCAM US-1641 MIDI In` and `MIDI Out`) with bidirectional byte translation.
  - Seamless automatic PipeWire/JACK graph bridging (`Midi-Bridge:TASCAM US-1641:MIDI In (capture)` and `MIDI Out (playback)`) for low-latency digital drum kits, keyboards, synthesizers, and DAWs.
  - Live MIDI I/O event counters in daemon telemetry (`tascamd.stats`).
  - Added `libasound2-dev` / `alsa-lib-devel` dependency checks to `install.sh` and build system.


### Added
- Full hardware MIDI Input and Output support for the US-1641's 5-pin DIN ports via USB bulk endpoints `EP 0x83` (MIDI IN) and `EP 0x04` (MIDI OUT) ([#7](https://github.com/Aerankas/tascam_us1641_linux/issues/7)):
  - Implemented 9-byte USB frame packetizer and depacketizer with `0xFD` pad filtering and `0xE0` delimiter alignment.
  - Native ALSA Sequencer integration (`TASCAM US-1641 MIDI In` and `MIDI Out`) with bidirectional byte translation.
  - Seamless automatic PipeWire/JACK graph bridging (`Midi-Bridge:TASCAM US-1641:MIDI In (capture)` and `MIDI Out (playback)`) for low-latency digital drum kits, keyboards, synthesizers, and DAWs.
  - Live MIDI I/O event counters in daemon telemetry (`tascamd.stats`).
  - Added `libasound2-dev` / `alsa-lib-devel` dependency checks to `install.sh` and build system.

### Fixed
- Fixed 40 Hz playback and capture sample-slip buzzing caused by evaluating raw quantum-sawtooth buffer depths against narrow deadbands; implemented low-pass filtered queue depth tracking with a wide $\pm 1024$-frame deadband ([#10](https://github.com/Aerankas/tascam_us1641_linux/issues/10)).
- Removed fractional-phase linear interpolation on playback and capture streams to eliminate phase flutter and granulation artifacts.
- Fixed random channel pair swapping (`In 01 ↔ In 02`, `In 09 ↔ In 10`) across daemon restarts by eliminating an erroneous `0x49, 0x0000, 0x0000` control transfer on shutdown that pushed an odd 15-frame packet into the hardware FIFO.
- Corrected FPGA bitplane parity mapping so physical front-panel line/guitar inputs and mic preamps map accurately to corresponding channels.
- Updated protocol specification documentation (`docs/protocol-spec.md`) with accurate bitplane parity mappings and closed-loop feedback mechanics.
- Fixed 32-bit signed integer overflow in Bulk IN frame parity decoder (`g_global_frame_idx`), which caused permanent capture silence after 6.2 hours of continuous streaming ([#5](https://github.com/Aerankas/tascam_us1641_linux/issues/5)).
- Added line buffering (`setlinebuf`) for `stdout` and `stderr` to ensure real-time logging in `systemd` user journals.
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

