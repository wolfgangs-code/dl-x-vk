#pragma once

#include <cstdint>
#include <vector>
#include "protocol.hpp"
#include "tile_engine.hpp"

namespace dl_turbo {

// Intermediate Frame Buffer (IFB) Geometry & Address Calculator
// Reconstructed from dl::nivo::dl3::encoder::IfbAddressCalculator (0x4d9890 / 0x4d99b0)
class IfbAddressCalculator {
public:
    IfbAddressCalculator(int width, int height, int bpp = 32);

    void ResetGeometry(int width, int height, int bpp = 32);
    size_t ComputeTileOffset(int tile_x, int tile_y) const;
    size_t GetTotalBufferSize() const { return m_total_size; }
    int GetStride() const { return m_stride; }

private:
    int m_width;
    int m_height;
    int m_bpp;
    int m_stride;
    size_t m_total_size;
};

// Compressed Frame Buffer (CFB) Bitrate Limiter
// Reconstructed from dl::nivo::dl3::codec::CfbBppLimitViolated (0x4c6c20)
class CfbBitBudget {
public:
    CfbBitBudget(uint32_t max_bits_per_pixel = 12);

    void SetLimit(uint32_t max_bpp) { m_max_bpp = max_bpp; }
    bool CheckBudget(size_t compressed_bytes, int pixel_count) const;

private:
    uint32_t m_max_bpp;
};

// DL3 / DL6 Tile Encoder
class Dl3Encoder {
public:
    Dl3Encoder();
    ~Dl3Encoder() = default;

    // Encodes a single 32bpp tile into compressed DisplayLink packet format
    bool EncodeTile(
        const uint8_t* raw_tile,
        const TileCoordinate& coord,
        std::vector<uint8_t>& out_packet,
        bool force_lossless = false
    );

    // Run-length encoding compressor
    static size_t CompressRle(
        const uint8_t* src,
        size_t src_len,
        uint8_t* dst,
        size_t dst_capacity
    );

private:
    IfbAddressCalculator m_ifb_calc;
    CfbBitBudget m_cfb_budget;
    std::vector<uint8_t> m_temp_buf;
};

} // namespace dl_turbo
