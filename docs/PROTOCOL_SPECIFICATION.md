# DisplayLink DL3 / DL6 Protocol Specification (Clean-Room Architecture)

This document provides a technical specification of the USB communication protocol and display pipeline for DisplayLink USB 3.0 graphics adapters (DL-3000, DL-5000, and DL-6000 Series).

---

## 1. USB Interface Architecture

DisplayLink devices (USB Vendor ID `0x17E9`) expose standard and vendor-specific interfaces:

### 1.1 Interfaces

| Interface ID | Class | Subclass | Protocol | Description |
|---|---|---|---|---|
| `0` | `0xFF` (Vendor) | `0x00` | `0x03` | Video Command & Control Interface |
| `1` | `0xFF` (Vendor) | `0x00` | `0x04` | High-speed Video Stream Interface |
| `2` | `0xFE` (App) | `0x01` | `0x01` | Device Firmware Upgrade (DFU) |
| `3..N` | `0x01` (Audio) | `0x01` / `0x02` | `0x20` | USB Audio Class 2.0 (UAC2) Stream |

### 1.2 Endpoint Assignments

| Endpoint | Type | Direction | Packet Size | Description |
|---|---|---|---|---|
| `EP 0` | Control | Bidirectional | 64 B | Standard USB enumeration & feature control |
| `EP 2` | Bulk | OUT | 1024 B | `ActiveCommandQueue`: Mode configuration, Heartbeats, State switches |
| `EP 4` | Bulk | IN | 1024 B | Status responses, EDID readbacks, Flow control ACKs |
| `EP 8` | Bulk | OUT | 1024 B | Head 0 (Display 1) Video Stream |
| `EP 10`| Bulk | OUT | 1024 B | Head 1 (Display 2) Video Stream |
| `EP 11`| Bulk | OUT | 1024 B | Auxiliary slice burst channel 0 |
| `EP 12`| Bulk | OUT | 1024 B | Auxiliary slice burst channel 1 |

---

## 2. Packet Framing Format

### 2.1 Command Packet Header (10 bytes)

Commands sent over `EP 2 OUT` follow a packed binary structure:

```
+-------------------------------------------------------+
| Sync Word (16-bit): 0xAF60                            |
+---------------------------+---------------------------+
| Head ID (8-bit)           | Opcode (8-bit)            |
+---------------------------+---------------------------+
| Sequence ID (16-bit)      | Payload Length (16-bit)   |
+---------------------------+---------------------------+
| Payload Data [0..N bytes]                             |
+-------------------------------------------------------+
```

#### Opcodes:
* `0x01`: `ResetReceiver` (Reset ASIC video decoder)
* `0x02`: `MapReset` (Reset internal memory mappings)
* `0x03`: `SetVideoMode` (Configure resolution, timing, refresh rate)
* `0x04`: `SetBlankState` (DPMS display power on/off)
* `0x05`: `SetGammaRamp` (Upload 256-entry RGB gamma LUT)
* `0x06`: `Heartbeat` (Periodic keepalive packet; prevents dock sleep)
* `0x10`: `FrameBegin` (Start of frame transmission)
* `0x11`: `SliceData` (Compressed tile data)
* `0x12`: `FrameCommit` (Display buffer flip on ASIC)

---

## 3. Video Pipeline & Color Space Conversion

### 3.1 ITU-R BT.601 Color Transformation

DisplayLink DL-6000 hardware processes display streams in planar YUV 4:2:0. Host drivers convert RGB framebuffers to YCbCr using fixed-point integer matrix arithmetic:

```
Y  = (( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16
Cb = ((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128
Cr = ((112 * R -  94 * G -  18 * B + 128) >> 8) + 128
```

Subsampling:
* **Y (Luma)**: Full resolution (1 sample per pixel).
* **Cb / Cr (Chroma)**: 2:1 horizontal and vertical subsampling (1 sample per 2×2 block).

### 3.2 Tile Decomposition

Frame updates are divided into macro-tiles (typically 16×16 or 32×32 pixels):
1. **Damage Tracking**: EVDI provides dirty rectangle bounds from the Linux DRM compositor.
2. **Coalescing**: Dirty rectangles are snapped to macro-tile boundaries.
3. **Temporal Differencing**: Tiles identical to the previous frame (determined via 64-bit checksum or hash) are discarded.
4. **Compression**: Changed tiles are compressed using RLE / entropy coding before transmission.

---

## 4. Hardware Keepalive Mechanism

To prevent the DisplayLink dock from entering USB low-power suspend (U1/U2) or blanking display outputs, the driver transmits a heartbeat packet every 500 ms:

```
[Sync: 0xAF60] [Head: 0x00] [Opcode: 0x06] [Uptime: uint32_t (ms)]
```
