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
 |   [TileEngine]     --> 32x32 Macro-tile grid & CRC32 differencing     |
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

| Color Space Engine | 4K Frame Throughput | Latency / Frame | CPU Overhead |
|:---|:---:|:---:|:---:|
| **Vulkan SPIR-V Compute (Kernel)** | **222+ FPS** | **~4.5 ms** | **~0% (GPU)** |
| **Vulkan End-to-End Pipeline** | **60+ FPS** | **~16.0 ms** | Minimal |
| **AVX2 SIMD Vectorized** | **100+ FPS** | **~9.9 ms** | 100% Core Load |
| **Original Scalar Reference** | ~18 FPS | ~55.0 ms | 100% Core Load |

*Benchmarks measured converting full 4K (3840×2160 @ 32bpp) frames to planar YUV420 on an AMD Ryzen 5 4500U APU (Radeon Vega graphics, Mesa RADV).*

---

## Features

* **Vulkan SPIR-V Compute Acceleration**: Offloads the entire ITU-R BT.601 RGB32-to-planar-YUV420 color space conversion directly to GPU compute units using host-visible/device-local memory, eliminating CPU bottlenecks.
* **Graceful Multi-Tier Fallback**: Automatically falls back to AVX2 SIMD instructions or scalar CPU paths if Vulkan compute is unavailable.
* **Temporal Macro-Tile Differencing**: Hardware SSE4.2 / CRC32 differencing filters unchanged tiles to drastically reduce USB transmission bandwidth.
* **Zero-Copy Architecture**: Uses host-coherent mapped storage buffers (SSBOs) with cached readback for optimal APU and discrete GPU memory bandwidth.
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
