# dl-x-vk: High-Performance Open DisplayLink Driver (Vulkan/SPIR-V Accelerated)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/standard-C%2B%2B20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Vulkan 1.2+](https://img.shields.io/badge/Vulkan-1.2%2B-red.svg)](https://www.vulkan.org)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.kernel.org)

**`dl-x-vk`** is an independent, high-performance open-source user-space display driver daemon for DisplayLink USB 3.0 graphics adapters and docking stations (DL-6000 / DL-5000 / DL-3000 series).

Designed to replace the proprietary, CPU-intensive `DisplayLinkManager` daemon, `dl-x-vk` leverages **Vulkan SPIR-V compute shaders** for hardware-accelerated color-space conversion, macro-tile damage differencing, and asynchronous zero-copy USB streaming via **`libusb-1.0`** and the Linux DRM **EVDI** interface.

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
 |   [EvdiDevice]     --> Framebuffer capture & dirty rect coalescing    |
 |         |                                                             |
 | [VulkanConverter]  --> SPIR-V Compute Shader Temporal Differencing    |
 |         |              (8,160 4K tiles compared in 3.5ms on GPU)      |
 |         |              (Fallback: SSE4.2 CRC32 differencing)          |
 |   [TileEngine]     --> 32x32 Macro-tile grid & dirty mask parsing     |
 |         |                                                             |
 | [VulkanConverter]  --> SPIR-V Compute Shader BT.601 RGB -> YUV420     |
 |         |              (Fallback: AVX2 SIMD / Scalar CPU)             |
 |   [Dl3Encoder]     --> Fast RLE / Entropy compression                 |
 |         |                                                             |
 |  [UsbTransport]    --> Asynchronous multi-URB USB streaming           |
 +-----------------------------------+-----------------------------------+
                                     | (Bulk Endpoints: EP 2 / EP 8 / EP 10)
                                     v
                      +-----------------------------+
                      | DisplayLink Hardware ASIC   |
                      |  (DL-6950, DL-5900, etc.)   |
                      +-----------------------------+
```

---

## Performance Highlights

| Subsystem / Kernel | 4K Throughput | Latency / Frame | CPU Overhead |
|:---|:---:|:---:|:---:|
| **Direct EVDI Zero-Copy Buffer Ingestion (100 Dirty Tiles)** | **755–1,221+ FPS** | **0.82–1.32 ms** | **0% (0 CPU memcpy)** |
| **Vulkan Clipped Dirty Incremental Update (100 Tiles)** | **324–736+ FPS** | **~1.36–3.09 ms** | **~0% (GPU Compute)** |
| **Vulkan GPU Compute Kernel (Zero-Copy)** | **188–475 FPS** | **~2.10–5.30 ms** | **~0% (GPU Compute)** |
| **Vulkan GPU Tile Differencing (8,160 Full 4K Tiles)** | **129–365 Passes/s** | **~2.74–7.74 ms** | **~0% (GPU Compute)** |
| **Vulkan GPU Indirect Dispatch Compression (Full Grid)** | **109–112+ FPS** | **~8.90 ms** | **~0% (GPU Compute)** |
| **Vulkan End-to-End Frame Pipeline** | **60–112 FPS** | **~8.87–16.66 ms** | Minimal |
| **AVX2 SIMD Color Conversion** | 100+ FPS | ~9.9 ms | 100% Core Load |
| **Original Scalar Reference** | ~18 FPS | ~55.0 ms | 100% Core Load |

*Benchmarks measured on local hardware: AMD Ryzen 5 4500U APU with AMD Radeon Graphics (RADV RENOIR, Mesa 26.2.4).*

---

## Features

* **Direct EVDI Zero-Copy Buffer Ingestion**: Registers Vulkan's mapped GPU input buffers (`m_buf_input[0]` and `m_buf_input[1]`) directly with the EVDI kernel module (`evdi_register_buffer`) using dual pre-bound descriptor sets. Compositor frame updates write straight into GPU memory, eliminating 100% of CPU framebuffer `memcpy` operations and achieving **1,221+ FPS (0.82 ms/frame)** throughput!
* **EVDI Damaged Bounding-Box Clipping**: Calculates tile-aligned damaged bounding boxes from EVDI dirty rectangles, restricting GPU compute workgroups and selective host row copies to the active region. Reduces CPU copy bandwidth by **98.5%** and accelerates dirty tile updates to **736+ FPS (1.36 ms)**.
* **GPU Indirect Dispatch (`vkCmdDispatchIndirect`)**: Seamlessly chains GPU Differencing $\to$ GPU DMA Command Setup $\to$ GPU Parallel Compression inside a single command buffer with zero host CPU roundtrips or mid-frame fence stalls.
* **End-to-End GPU Pipeline**: Zero CPU-to-GPU and GPU-to-CPU uncompressed frame swaps. The frame stays in GPU memory for differencing and compression.
* **GPU Parallel Tile Compression**: Compresses dirty $32 \times 32$ macro-tiles directly on the GPU in parallel into DisplayLink DL1/DL3 USB packet format using atomic byte reservation, writing output directly into a host-cached packet buffer.
* **Vulkan SPIR-V Compute Acceleration**: Offloads ITU-R BT.601 RGB32-to-planar-YUV420 color space conversion directly to GPU compute units using host-visible/device-local memory, eliminating CPU bottlenecks.
* **GPU Temporal Macro-Tile Differencing**: Compares entire 4K display frames against reference frames on GPU in ~2.7 ms, isolating dirty tiles with zero CPU intervention.
* **Graceful Multi-Tier Fallback**: Automatically falls back to SSE4.2 CRC32 differencing, AVX2 SIMD instructions, or scalar CPU paths if Vulkan compute is unavailable.
* **Clean-Room Open Source**: 100% C++20 implementation under the MIT License without proprietary binary blobs.

---

## Prerequisites & Dependencies

On Arch Linux / CachyOS / Manjaro:
```bash
sudo pacman -S base-devel cmake evdi-dkms libusb vulkan-icd-loader
```

On Ubuntu / Debian:
```bash
sudo apt update
sudo apt install build-essential cmake libusb-1.0-0-dev libevdi-dev libvulkan-dev glslc
```

Ensure the `evdi` kernel module is loaded:
```bash
sudo modprobe evdi
```

---

## Building from Source

Vulkan headers are vendored in `include/`, so the build works out-of-the-box on any system with a standard C++20 compiler and Vulkan loader:

```bash
git clone https://github.com/wolfgangs-code/dl-x-vk.git
cd dl-x-vk

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## Usage

### Running the Performance Benchmark
```bash
./dl-x-vk --benchmark
```
*Transforms 100 consecutive 4K (3840×2160) frames to measure sustained GPU throughput and end-to-end latency.*

### Running the Service Daemon
```bash
# Foreground execution with logging
./dl-x-vk -logging

# Enable verbose debug tracing
./dl-x-vk -debug

# Show version info
./dl-x-vk -version
```

---

## Shaders & Compute Pipeline

The GLSL compute shader (`src/shaders/rgb_to_yuv420.comp`) processes 2×2 pixel macro-quads per thread in 16×16 workgroups (256 threads / 32×32 pixels per workgroup).

To recompile the shader manually:
```bash
glslc -mfmt=c src/shaders/rgb_to_yuv420.comp -o src/shaders/rgb_to_yuv420_spv.inc
```
*(CMake automatically recompiles shaders if `glslc` is detected).*

---

## License

This project is licensed under the [MIT License](LICENSE).
