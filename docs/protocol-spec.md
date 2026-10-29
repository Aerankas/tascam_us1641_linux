# TASCAM US-1641 USB 2.0 Protocol Specification

This document details the reverse-engineered USB 2.0 High-Speed initialization, clocking, bitplane capture multiplexing, and closed-loop isochronous playback feedback protocol of the **TASCAM US-1641** (`0644:8017`).

---

## 1. USB Device Descriptors & Endpoints

- **Vendor ID**: `0x0644` (TEAC Corp. / TASCAM)
- **Product ID**: `0x8017` (US-1641)
- **USB Speed**: High-Speed (`480 Mbps`)
- **Configuration**: `1`
- **Interfaces**:
  - `Interface 0, AltSetting 1`: Control, Audio Playback & MIDI Interface
  - `Interface 1, AltSetting 1`: Audio Capture & Isochronous Feedback Interface

### Active Endpoints

| Endpoint | Interface | Direction | Transfer Type | Max Packet Size | Interval | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`0x02`** | `0` | `OUT` | Isochronous | `156 bytes` | `bInterval = 1` (125 µs) | 4-channel 24-bit playback audio stream |
| **`0x83`** | `0` | `IN` | Bulk | `512 bytes` | N/A | Hardware MIDI Input (9-byte frames) |
| **`0x04`** | `0` | `OUT` | Bulk | `512 bytes` | N/A | Hardware MIDI Output (9-byte frames) |
| **`0x81`** | `1` | `IN` | Isochronous (Async) | `64 bytes` | `bInterval = 4` (1 ms) | Hardware DAC sample consumption feedback tokens |
| **`0x86`** | `1` | `IN` | Bulk | `512 bytes` | N/A | 16-channel bitplane-multiplexed capture stream |

---

## 2. Hardware Initialization Sequence

To wake the FPGA and start the internal ADC/DAC clocks, the host must claim Interfaces `0` and `1` (setting both to `AltSetting 1`) and issue the following vendor control transfers:

1. **Firmware & Status Handshake**:
   - `bmRequestType = 0xC0`, `bRequest = 0x56`, `wValue = 0x0000`, `wIndex = 0x0000`, `wLength = 15` (Read twice; returns ASCII firmware string at offset 3).
   - `bmRequestType = 0xC0`, `bRequest = 0x49`, `wValue = 0x0000`, `wIndex = 0x0000`, `wLength = 1` (Read status byte).
   - `bmRequestType = 0x40`, `bRequest = 0x49`, `wValue = 0x0000`, `wIndex = 0x0002`, `wLength = 0`.
2. **Endpoint Sample Rate Programming (`0x22` Class Control)**:
   - 3-byte little-endian sample rate payload:
     - `44,100 Hz`: `{0x44, 0xAC, 0x00}`
     - `48,000 Hz`: `{0x80, 0xBB, 0x00}`
   - Sent to both `wIndex = 0x0086` (Capture EP) and `wIndex = 0x0002` (Playback EP) via `bmRequestType = 0x22`, `bRequest = 0x01`, `wValue = 0x0100`.
3. **FPGA Clock & Routing Register Sequence (`bRequest = 0x41`)**:
   - Read 5 bytes: `bmRequestType = 0xC0`, `bRequest = 0x41`, `wValue = 0x0D00`, `wIndex = 0x0101`.
   - Write registers (`bmRequestType = 0x40`, `bRequest = 0x41`, `wIndex = 0x0101`, `wLength = 0`):
     - `wValue = 0x0D04`
     - `wValue = 0x0E00`
     - `wValue = 0x0F00`
     - `wValue = <clock_reg>`:
       - `0x1000` = `44,100 Hz`
       - `0x1002` = `48,000 Hz`
       - `0x1008` = `88,200 Hz`
       - `0x100A` = `96,000 Hz`
     - `wValue = 0x110B`
4. **Re-confirm Endpoint Sample Rates & Start Stream Engine**:
   - Repeat the 3-byte `0x22` rate transfers to `0x0086` and `0x0002`.
   - Start streaming: `bmRequestType = 0x40`, `bRequest = 0x49`, `wValue = 0x0032`, `wIndex = 0x0000`, `wLength = 0`.

---

## 3. Capture Protocol (`EP 0x86 Bulk IN` Bitplane Multiplexing)

Unlike standard interleaved PCM, the US-1641 FPGA serializes 14 analog ADC channels in parallel across the data bus. Each 32-byte frame represents **one sample period for 7 channels**, alternating between **EVEN** and **ODD** frames:

- **Bytes `0..23` (24 rows)**: Bitplane rows from **MSB (Bit 23 at Byte 0)** down to **LSB (Bit 0 at Byte 23)**.
- **Bits `0..6` of each byte**: 7 analog channels:
  - **EVEN Frame (`frame_idx % 2 == 0`)**:
    - Bit `b` (`0..6`) $\rightarrow$ Channel `2 * b` (`In 1, 3, 5, 7, 9, 11, 13`)
  - **ODD Frame (`frame_idx % 2 == 1`)**:
    - Bit `b` (`0..6`) $\rightarrow$ Channel `2 * b + 1` (`In 2, 4, 6, 8, 10, 12, 14`)
- **Bytes `24..31` (8 bytes)**: Aux / S/PDIF status and digital audio framing (`0x00 00 00 80` on EVEN frames, `0x00 00 00 40` on ODD frames).

### Demuxing Algorithm (C Reference)

```c
for (int b = 0; b < 7; b++) {
    int ch_idx = (parity == 0) ? (2 * b) : (2 * b + 1); // 0..13
    int32_t val = 0;
    for (int bit = 0; bit < 24; bit++) {
        val = (val << 1) | ((frame[bit] >> b) & 1);
    }
    if (val & 0x800000) val -= 0x1000000; // Sign-extend 24-bit to 32-bit
    sample_s16[ch_idx] = (int16_t)(val >> 8);
}
```

Once an `EVEN` + `ODD` pair (64 bytes total) is processed, one full 16-channel sample frame is pushed into the lock-free capture ring buffer.

---

## 4. Playback Protocol (`EP 0x02 Isochronous OUT` & `EP 0x81 Feedback IN`)

### Sample Packing (`EP 0x02 OUT`)

Playback audio is standard interleaved 4-channel **24-bit signed little-endian PCM** (`12 bytes` per multi-channel sample frame):
- Bytes `0..2`: Channel 1 (`Main Out L / Phones L`)
- Bytes `3..5`: Channel 2 (`Main Out R / Phones R`)
- Bytes `6..8`: Channel 3 (`Line Out 3`)
- Bytes `9..11`: Channel 4 (`Line Out 4`)

### Closed-Loop DAC Synchronization (`EP 0x81 IN`)

Each `EP 0x02` transfer spans `40` USB High-Speed microframes (`40 * 125 µs = 5 ms`):
- At **48,000 Hz**, 5 ms corresponds to nominally **240 samples** (`6 samples` per microframe $\times 40$).
- At **44,100 Hz**, 5 ms corresponds to **220.5 samples** (alternating `220` and `221` samples per transfer).

Because the host motherboard's USB SOF timer and the US-1641's internal crystal oscillator inevitably differ by ~10–50 ppm, open-loop transmission will slowly overflow or underflow the hardware DAC's 64-sample FIFO every ~50–60 seconds.

To prevent FIFO slip, `EP 0x81` (`bInterval = 4`, 1 ms period) returns a 3-byte packet `[count(t), count(t-1), count(t-2)]` where `Byte 0` reports the exact number of samples consumed by the physical DAC during the last millisecond (`~48` at 48 kHz, `44` or `45` at 44.1 kHz).

`tascamd` accumulates `g_hw_consumed_samples` from `EP 0x81` and compares it against `g_hw_sent_samples` on `EP 0x02`, dynamically adjusting each 5 ms transfer between `239 / 240 / 241` samples (at 48 kHz) or `220 / 221` samples (at 44.1 kHz) to maintain a zero-drift lock with the physical DAC crystal.

---

## 5. MIDI Protocol (`EP 0x83 Bulk IN` & `EP 0x04 Bulk OUT`)

The US-1641 features standard 5-pin DIN MIDI IN and MIDI OUT ports routed over dedicated USB Bulk endpoints on Interface 0:
- **`EP 0x83` (Bulk IN)**: Hardware MIDI Input.
- **`EP 0x04` (Bulk OUT)**: Hardware MIDI Output.

### Frame Structure & Framing (9-Byte Packets)

Both input and output streams use fixed 9-byte packet framing:
- **Bytes `0..7` (8 bytes)**: Raw MIDI message payload (Note-On, Note-Off, CC, Aftertouch, Active Sensing, SysEx, etc.).
  - Unused or idle byte slots within the 8-byte payload are padded with `0xFD`.
- **Byte `8` (1 byte)**: Frame delimiter byte, set to `0x00`.

```
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
| Byte 0 | Byte 1 | Byte 2 | Byte 3 | Byte 4 | Byte 5 | Byte 6 | Byte 7 | Byte 8 |
|   M0   |   M1   |   M2   |  0xFD  |  0xFD  |  0xFD  |  0xFD  |  0xFD  |  0x00  |
+--------+--------+--------+--------+--------+--------+--------+--------+--------+
 <---------------- Raw MIDI Payload (Padded with 0xFD) ---------------> <Delim>
```

### Stream Demuxing & ALSA Sequencer Bridging

1. **MIDI Input (`EP 0x83 IN`)**:
   - `tascamd` maintains 4 asynchronous bulk transfers on `EP 0x83`.
   - Incoming byte streams are buffered in an accumulator to handle arbitrary transfer boundaries.
   - For every 9-byte frame ending in `0xE0`, bytes `0..7` are scanned:
     - Any byte matching `0xFD` is discarded as idle padding.
     - Any byte $\ne$ `0xFD` is passed to the ALSA sequencer byte encoder (`snd_midi_event_encode_byte`).
   - Reconstructed ALSA sequencer events are emitted directly to the `TASCAM US-1641 MIDI In` port.

2. **MIDI Output (`EP 0x04 OUT`)**:
   - A dedicated worker thread polls the `TASCAM US-1641 MIDI Out` sequencer port.
   - Outgoing ALSA sequencer events are decoded into raw MIDI byte streams (`snd_midi_event_decode`).
   - Bytes are packetized into 8-byte chunks (padded with `0xFD` to fill 8 bytes), followed by `0xE0`, and transmitted to `EP 0x04`.

3. **Sound Server Integration**:
   - PipeWire automatically discovers the ALSA Sequencer client and creates `Midi-Bridge:TASCAM US-1641:MIDI In (capture)` and `Midi-Bridge:TASCAM US-1641:MIDI Out (playback)`.
   - DAWs and software synthesizers (e.g. Reaper, Ardour, Bitwig, Carla, Hydrogen) can connect to the MIDI nodes with zero configuration.
