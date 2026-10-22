# TASCAM US-1641 USB 2.0 Protocol Specification

This document details the reverse-engineered USB 2.0 High-Speed initialization, clocking, bitplane capture multiplexing, and closed-loop isochronous playback feedback protocol of the **TASCAM US-1641** (`0644:8017`).

---

## 1. USB Device Descriptors & Endpoints

- **Vendor ID**: `0x0644` (TEAC Corp. / TASCAM)
- **Product ID**: `0x8017` (US-1641)
- **USB Speed**: High-Speed (`480 Mbps`)
- **Configuration**: `1`
- **Interfaces**:
  - `Interface 0, AltSetting 1`: Control & Command Interface
  - `Interface 1, AltSetting 1`: Audio Streaming Interface

### Active Endpoints (Interface 1, AltSetting 1)

| Endpoint | Direction | Transfer Type | Max Packet Size | Interval | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`0x81`** | `IN` | Isochronous (Async) | `64 bytes` | `bInterval = 4` (1 ms) | Hardware DAC sample consumption feedback tokens |
| **`0x02`** | `OUT` | Isochronous | `96 bytes` | `bInterval = 1` (125 µs) | 4-channel 24-bit playback audio stream |
| **`0x86`** | `IN` | Bulk | `512 bytes` | N/A | 16-channel bitplane-multiplexed capture stream |

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
