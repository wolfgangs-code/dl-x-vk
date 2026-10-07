# dl-x-vk: High-Performance Open DisplayLink Driver

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/standard-C%2B%2B20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.kernel.org)

**`dl-x-vk`** is an independent, open-source user-space display driver daemon for DisplayLink USB 3.0 graphics adapters and docking stations (DL-6000 / DL-5000 / DL-3000 series).

Designed to replace the proprietary, CPU-intensive `DisplayLinkManager` daemon, `dl-x-vk` interfaces directly with Linux DRM via **EVDI** and drives DisplayLink hardware asynchronously via **`libusb-1.0`**, featuring SIMD-vectorized ITU-R BT.601 color-space conversion, macro-tile damage differencing, and low-latency USB bulk streaming.

---

## Architecture Overview

```
                      +-----------------------------+
                      |   X11 / Wayland Compositor  |
                      +--------------+--------------+
                                     | (DRM / KMS)
                                     v
                      +-----------------------------+
                      |   evdi.ko Kernel Module     |
                      +--------------+--------------+
                                     | (libevdi)
                                     v
 +-----------------------------------------------------------------------+
 | dl-x-vk User Daemon                                                   |
 |                                                                       |
 |   [EvdiDevice]  --> Double-buffered framebuffer capture & damage rects|
 |          |                                                            |
 |   [TileEngine]  --> 32x32 Macro-tile grid & damage coalescing         |
 |          |                                                            |
 | [ColorConverter]--> SIMD-vectorized BT.601 RGB -> Planar YUV420       |
 |          |                                                            |
 |   [Dl3Encoder]  --> Fast RLE / Entropy compression                    |
 |          |                                                            |
 |  [UsbTransport] --> Asynchronous multi-URB zero-copy USB submission   |
 +-----------------------------------+-----------------------------------+
                                     | (Bulk Endpoints: EP 2 / EP 8 / EP 10)
                                     v
                      +-----------------------------+
                      | DisplayLink Hardware ASIC   |
                      |  (DL-6950, DL-5900, etc.)   |
                      +-----------------------------+
```

---

## Features

* **High Performance**: Vectorized color conversion pipeline capable of processing 4K60 video at **>60 FPS** on standard multi-core processors.
* **Low Latency**: Asynchronous USB transfer pipeline eliminating controller pipeline stalls.
* **EVDI Integration**: Full compatibility with the standard Linux [`libevdi`](https://github.com/DisplayLink/evdi) kernel interface.
* **Clean & Open**: 100% open-source C++20 codebase without proprietary blobs or emulated Windows registry layers.
* **Hardware Support**: Targets DisplayLink DL-6000 series (e.g. DL-6950 dual 4K docks like Lenovo ThinkPad Hybrid USB-C/A Dock), DL-5000 series, and DL-3000 series.

---

## Prerequisites & Dependencies

On Arch Linux / CachyOS:
```bash
sudo pacman -S base-devel cmake evdi-dkms libusb
```

On Ubuntu / Debian:
```bash
sudo apt update
sudo apt install build-essential cmake libusb-1.0-0-dev libevdi-dev
```

Ensure the `evdi` kernel module is loaded:
```bash
sudo modprobe evdi
```

---

## Building from Source

```bash
git clone https://github.com/wolfgangs-code/dl-x-vk.git
cd dl-x-vk

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## Usage

### Running the Service
```bash
# Foreground execution with logging
./dl-x-vk -logging

# Enable verbose debug tracing
./dl-x-vk -debug

# Show version
./dl-x-vk -version
```

### Running the Performance Benchmark
```bash
./dl-x-vk --benchmark
```
*Benchmarking transforms 100 consecutive 4K (3840×2160) frames to measure sustained pixel throughput and latency.*

---

## Protocol & Specification

For details on packet framing, USB endpoint routing, and hardware keepalive packets, see [docs/PROTOCOL_SPECIFICATION.md](docs/PROTOCOL_SPECIFICATION.md).

---

## License

This project is licensed under the [MIT License](LICENSE).
