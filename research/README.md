# DisplayLink Turbo: Research & Standalone Prototype Archive

This directory archives the standalone replacement daemon prototype (`dl-x-vk`) and associated experimental components developed prior to adopting the **Option 1 Hybrid Acceleration Shim** architecture.

## Why These Are in Research

On modern DisplayLink USB docks (specifically the DL-6950 ASIC, such as in the ThinkPad Hybrid USB-C/A Dock `17e9:6015`), the physical HDMI and DisplayPort PHY outputs remain powered down and inactive until an **`ECJPAKE` cryptographic session handshake** is completed over USB bulk endpoints.

Without Synaptics' proprietary crypto keys, a standalone open-source driver cannot complete this authentication handshake, leaving physical monitors blank.

Rather than reverse-engineering the cryptographic session handshake (which poses significant DMCA §1201 / anti-circumvention and HDCP legal concerns), we transitioned to the **Hybrid Acceleration Shim (`libevdi_turbo.so`)**, which allows the official `DisplayLinkManager` daemon to authenticate and drive the hardware while offloading dirty-tile detection to the AMD GPU via Vulkan.

## Archived Components

### `standalone_daemon/`
- **`main.cpp`**: Standalone multi-head daemon CLI, DRM discovery, and EDID loading.
- **`service.cpp` / `service.hpp`**: Systemd service integration loop.
- **`evdi_device.cpp` / `evdi_device.hpp`**: Direct EVDI kernel driver interface and dumb-buffer allocator.
- **`usb_transport.cpp` / `usb_transport.hpp`**: Custom `libusb-1.0` asynchronous bulk transfer queue and interface 0 management.
- **`codec_dl3.cpp` / `codec_dl3.hpp`**: CPU DL3 Huffman/RLE compression engine.
- **`color_converter.cpp` / `color_converter.hpp`**: CPU AVX2/SSE ARGB-to-YUV420 color conversion.
- **`protocol.hpp`**: DisplayLink USB wire protocol headers and command packet structs.

### `shaders/`
- **`rgb_to_yuv420.comp`**: Full-screen GPU compute shader for BT.601/BT.709 color conversion.
- **`tile_compression.comp`**: Experimental SPIR-V compute shader for GPU-side entropy compression.
