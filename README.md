# TASCAM US-1641 Linux Driver & Control Panel (`tascamd`)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Audio: PipeWire](https://img.shields.io/badge/Audio-PipeWire%20%2F%20JACK-green.svg)](https://pipewire.org/)
[![Hardware: USB 0644:8017](https://img.shields.io/badge/USB-0644%3A8017-orange.svg)](#hardware-overview)

A full-duplex, low-latency **PipeWire audio bridge daemon (`tascamd`)** and **Graphical Hardware Control Panel** for the **TASCAM US-1641** 16x4 USB 2.0 Studio Audio Interface on Linux.

---

## Why This Project Exists

The TASCAM US-1641 (`0644:8017`) uses a proprietary USB 2.0 High-Speed protocol rather than standard USB Audio Class (UAC1/UAC2):
- **Capture (`EP 0x86 Bulk IN`)** transmits 14 analog inputs multiplexed as **transposed vertical bitplanes** across alternating 32-byte `EVEN`/`ODD` frames.
- **Playback (`EP 0x02 Isochronous OUT`)** requires closed-loop pacing synchronized against 1 ms hardware DAC frame consumption tokens from **`EP 0x81 Isochronous IN`**.

Historically, the experimental Linux kernel `snd-usb-us122l` driver failed on modern xHCI (USB 3.x) host controllers (`usb_stream_start` / `-ENOSPC` bandwidth allocation errors) and suffered from periodic FIFO wrap distortion.

**`tascamd`** solves this completely in user space using `libusb-1.0` and native `libpipewire-0.3` real-time nodes—unlocking all **16 inputs and 4 outputs** across desktop apps and professional DAWs (Ardour, Reaper, Bitwig, Audacity) with zero kernel patching required.

---

## Key Features

- **Full 16-Input / 4-Output Studio Routing**:
  - **Inputs 1–8**: Front-panel XLR Microphone preamps (`Mic 1` – `Mic 8`)
  - **Inputs 9–10**: Front-panel 1/4" Line / Instrument (`GUITAR`) inputs (`Line 9` – `Line 10`)
  - **Inputs 11–14**: Rear-panel 1/4" balanced Line inputs (`Line 11` – `Line 14`)
  - **Inputs 15–16**: Rear-panel S/PDIF stereo digital inputs
  - **Outputs 1–2**: Main Monitor L/R & Front Headphones
  - **Outputs 3–4**: Balanced Line Outputs 3 & 4
- **Native 48.0 kHz & 44.1 kHz Hardware Clocking**:
  - Defaults to **48,000 Hz** to match PipeWire's native graph clock for 1:1 bit-perfect streaming with zero real-time software resampling overhead.
  - Supports **44,100 Hz** for legacy CD audio workflows.
- **Closed-Loop DAC Feedback Governor (`EP 0x81`)**:
  - Tracks real-time hardware DAC sample consumption tokens every millisecond, locking host USB output 1:1 to the TASCAM's internal crystal oscillator with $\pm 1$ sample accuracy—eliminating long-term clock drift and FIFO wrap distortion.
- **Asynchronous Bulk Capture Pipeline**:
  - 4-stage pipelined asynchronous USB bulk transfers (`16 KB` chunks) paired with a 32.32 fixed-point fractional linear resampler, preventing capture dropouts during multi-track recording.
- **Hardware MIDI Input & Output (5-Pin DIN)**:
  - Bridges the US-1641's hardware MIDI IN (`EP 0x83`) and MIDI OUT (`EP 0x04`) ports directly to the Linux ALSA Sequencer (`TASCAM US-1641 MIDI In` and `MIDI Out`).
  - Automatically mapped into the PipeWire / JACK graph as `Midi-Bridge:TASCAM US-1641:MIDI In (capture)` and `MIDI Out (playback)`.
  - Connect digital drum kits, keyboard controllers, synthesizers, and drum machines directly to DAWs and virtual instruments (Reaper, Ardour, Hydrogen, Superior Drummer, EZDrummer) with ultra-low latency.
- **Graphical Hardware Control Panel (PyQt6)**:
  - Native desktop GUI for switching sample rates (`48 kHz` / `44.1 kHz`), adjusting PipeWire buffer size / latency quantum (`64` to `1024` samples), tuning pre-roll jitter buffers, and monitoring live DAC lock & dropout telemetry.

---

## Quick Start Installation

### 1. Clone & Run the Automated Installer

Supports **Ubuntu / Kubuntu / Debian** (`apt`), **Fedora** (`dnf`), and **Arch Linux** (`pacman`):

```bash
git clone https://github.com/Aerankas/tascam_us1641_linux.git
cd tascam_us1641_linux
./install.sh
```

What `./install.sh` does:
1. Installs required build and runtime packages (`gcc`, `pkg-config`, `libpipewire-0.3-dev`, `libusb-1.0-0-dev`, `python3-pyqt6`, `qpwgraph`).
2. Installs the USB permission rule (`/etc/udev/rules.d/90-tascam-us1641.rules`) so `tascamd` runs safely as your regular user without `root`.
3. Compiles `tascamd` and installs `tascamd` + `tascam-control-panel` into `~/.local/bin/`.
4. Installs and enables the `tascamd.service` systemd user service.
5. Adds **TASCAM US-1641 Control Panel** to your desktop application launcher.

### Manual Build (`Makefile`)

If you prefer manual steps:

```bash
make                  # Compile build/tascamd
make install-udev     # Install /etc/udev/rules.d/90-tascam-us1641.rules (sudo)
make enable           # Install binary, GUI, desktop file, and enable systemd service
```

---

## Using the Graphical Control Panel

Launch **TASCAM US-1641 Control Panel** from your desktop application menu (under *Multimedia* or *Settings*), or run:

```bash
~/.local/bin/tascam-control-panel
```

From the Control Panel you can:
- **Verify Hardware & Clock Lock**: Check live USB connection status, systemd daemon state, active sample rate, and `EP 0x81` DAC servo lock.
- **Change Sample Rate**: Switch between `48,000 Hz (Studio Default)` and `44,100 Hz (CD Standard)`.
- **Adjust Buffer Latency (PipeWire Quantum)**:
  - `64` / `128` samples (`1.3 ms` – `2.7 ms`): Ultra-low latency for live software amp/synth monitoring.
  - `256` samples (`5.3 ms`): Recommended for multi-track recording.
  - `512` / `1024` samples (`10.7 ms` – `21.3 ms`): High-stability buffer for heavy mixing sessions.
- **Open Visual Patchbay (`qpwgraph`)**: Visually inspect and route all 16 input channels and 4 output channels.

---

## Setting Up Your DAW (Ardour, Reaper, Bitwig)

Because `tascamd` registers native physical `Audio/Source` (16-channel) and `Audio/Sink` (4-channel) nodes in **PipeWire**, your DAW can access all 16 independent inputs and 4 outputs through **PipeWire-JACK** (`pw-jack`).

### 1. Launch Your DAW with `pw-jack`

Run your DAW via `pw-jack` so it sees the full 16x4 studio port matrix:

```bash
pw-jack ardour9
# or: pw-jack reaper
```

*(Tip: In Ardour or Reaper's **Audio/MIDI Setup** dialog, select **JACK** as the Audio System. It will automatically lock to PipeWire at **48.0 kHz**).*

### 2. Patching Inputs in Ardour

1. Open **Window $\rightarrow$ Audio Connections** (`Alt + P`).
2. Click the **Sources** tab on the left and select **Audio** on the top.
3. Expand **TASCAM US-1641 Inputs**:
   - `In 01 (Mic 1)` – `In 08 (Mic 8)`: Front XLR Mic preamps
   - `In 09 (Line 9)` – `In 14 (Line 14)`: 1/4" Guitar / Line inputs
   - `In 15 (SPDIF L)` – `In 16 (SPDIF R)`: Digital inputs
4. Click the matrix intersections to route any physical input to your Ardour track inputs.

### 3. Hardware Direct Monitoring vs. Software Monitoring

The TASCAM US-1641 features a front-panel **`MIX` (`INPUT` $\leftrightarrow$ `COMPUTER`)** balance knob:
- **Zero-Latency Hardware Monitoring**: Blend `INPUT` and `COMPUTER` on the front panel to hear your dry microphone/guitar signal directly from the hardware preamps with zero latency. *(Ensure your DAW track is set to monitor **Disk** rather than **In** so you don't hear a doubled signal).*
- **Live Software Plugin Monitoring (Amp Sims / Reverb)**: Turn the front-panel `MIX` knob fully clockwise to **`COMPUTER`**, and enable **Input Monitoring (`In`)** on your DAW track.

### 4. Routing MIDI Instruments & Digital Drum Kits

The US-1641's physical 5-pin DIN **MIDI IN** and **MIDI OUT** ports appear automatically in ALSA and PipeWire:
- **MIDI Input**: `Midi-Bridge:TASCAM US-1641:MIDI In (capture)`
- **MIDI Output**: `Midi-Bridge:TASCAM US-1641:MIDI Out (playback)`

To connect a digital drum kit or keyboard synthesizer:
1. Connect a standard 5-pin DIN MIDI cable from your drum module's **MIDI OUT** port to the TASCAM US-1641 front-panel **MIDI IN** port.
2. In your DAW (Reaper, Ardour, Bitwig) or drum sampler (Hydrogen, Carla, sfizz, EZDrummer / Superior Drummer via Wine), create an Instrument/MIDI track.
3. Select **TASCAM US-1641 MIDI In** as the MIDI input device.
4. Arm the track for live monitoring or recording to trigger drum samplers with zero latency.

---

## Hardware & USB Best Practices

- **Connect to a Direct Motherboard USB Port**:
  Because the US-1641 streams high-bandwidth USB 2.0 High-Speed isochronous and bulk transfers continuously, connect it directly to a rear motherboard root port rather than chaining it through an unpowered external USB hub shared with high-polling-rate gaming mice or webcams.

---

## Command-Line Usage & Configuration

Configuration is stored in `~/.config/tascamd.conf`:

```ini
# TASCAM US-1641 Hardware Bridge Configuration
sample_rate = 48000
preroll_frames = 2048
```

You can also run `tascamd` directly from the terminal with CLI overrides:

```bash
~/.local/bin/tascamd --help
```

```text
Usage: tascamd [OPTIONS]

Full-Duplex PipeWire Audio Bridge Daemon for TASCAM US-1641 (v1.0.0)

Options:
  -r, --rate <48000|44100>   Override hardware sample rate (default: 48000)
  -p, --preroll <frames>     Override jitter pre-roll buffer frames (256..8192, default: 2048)
  -d, --debug-log            Write continuous CSV telemetry to runtime directory
  -v, --version              Print version and exit
  -h, --help                 Show this help message
```

### Useful Service Commands

```bash
# Check daemon status
systemctl --user status tascamd

# View live logs
journalctl --user -u tascamd -f

# Restart daemon
systemctl --user restart tascamd

# Uninstall user-space components
./uninstall.sh
```

---

## Technical Protocol Documentation

For kernel and audio developers interested in the reverse-engineered USB control sequences, bitplane multiplexing layout, and `EP 0x81` feedback token protocol, see [`docs/protocol-spec.md`](docs/protocol-spec.md).

---

## License

Released under the [MIT License](LICENSE).
