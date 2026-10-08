#pragma once

#include <cstdint>
#include <cstddef>

namespace dl_turbo {
namespace protocol {

// USB Endpoints for DisplayLink DL-6000 / DL-5000 / DL-3000 Series
constexpr uint8_t EP_CMD_OUT        = 0x02; // Bulk OUT: Command & Control channel
constexpr uint8_t EP_STATUS_IN      = 0x84; // Bulk IN:  Status, EDID, ACK channel
constexpr uint8_t EP_VIDEO_HEAD0    = 0x08; // Bulk OUT: Video Stream Head 0
constexpr uint8_t EP_VIDEO_HEAD1    = 0x0a; // Bulk OUT: Video Stream Head 1
constexpr uint8_t EP_SLICE_BURST_0  = 0x0b; // Bulk OUT: High-speed video slice channel 0
constexpr uint8_t EP_SLICE_BURST_1  = 0x0c; // Bulk OUT: High-speed video slice channel 1

// Interface definitions
constexpr int INTERFACE_VIDEO_CONTROL = 0;
constexpr int INTERFACE_VIDEO_STREAM  = 1;
constexpr int INTERFACE_DFU           = 2;

// Standard DL3/DL6 Packet Constants
constexpr uint16_t DL_SYNC_WORD     = 0xAF60;
constexpr uint32_t DL_FRAME_MAGIC   = 0x52464C44; // 'DLFR' (DisplayLink Frame)
constexpr uint32_t DL_RESET_ECHO    = 0x54534552; // 'REST'

// Command Opcodes
enum class CommandOpcode : uint8_t {
    ResetReceiver   = 0x01,
    MapReset        = 0x02,
    SetVideoMode    = 0x03,
    SetBlankState   = 0x04,
    SetGammaRamp    = 0x05,
    Heartbeat       = 0x06,
    FrameBegin      = 0x10,
    SliceData       = 0x11,
    FrameCommit     = 0x12,
    I2cReadRequest  = 0x20,
    I2cWriteRequest = 0x21,
    EdidReadRequest = 0x22,
};

// Compression Formats
enum class TileCompression : uint8_t {
    RawUncompressed = 0x00,
    RunLengthDL1    = 0x01,
    EntropyDL3      = 0x02,
    PlanarYuv420DL6 = 0x03,
};

#pragma pack(push, 1)

// Command Packet Header
struct CommandPacketHeader {
    uint16_t sync_word;       // 0xAF60
    uint8_t  head_id;         // Head / Display Index (0 or 1)
    uint8_t  opcode;          // CommandOpcode
    uint16_t sequence_id;     // Monotonic counter
    uint16_t payload_length;  // Length of following payload bytes
};

// Mode Configuration Packet Payload
struct ModeConfigPayload {
    uint16_t width;
    uint16_t height;
    uint16_t refresh_rate;
    uint8_t  color_depth;     // e.g. 24 or 32
    uint8_t  pixel_format;    // PixelFormat FourCC code
    uint32_t pixel_clock_khz;
    uint16_t h_sync_start;
    uint16_t h_sync_end;
    uint16_t h_total;
    uint16_t v_sync_start;
    uint16_t v_sync_end;
    uint16_t v_total;
};

// Frame Container Header (sent before dirty tile sequence)
struct FrameSectionHeader {
    uint32_t magic;           // 'DLFR'
    uint32_t frame_index;
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t tile_count;
    uint8_t  head_id;
    uint8_t  reserved;
};

// Individual Tile Descriptor Header
struct TileDescriptorHeader {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t  compression;     // TileCompression
    uint8_t  flags;           // Bit 0: Lossless, Bit 1: Keyframe
    uint32_t compressed_bytes;
};

// Heartbeat Keepalive Packet
struct KeepAlivePacket {
    uint16_t sync_word;       // 0xAF60
    uint8_t  head_id;
    uint8_t  opcode;          // CommandOpcode::Heartbeat
    uint32_t uptime_ms;
};

#pragma pack(pop)

} // namespace protocol
} // namespace dl_turbo
