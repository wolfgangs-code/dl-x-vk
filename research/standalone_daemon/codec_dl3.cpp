#include "codec_dl3.hpp"
#include <cstring>
#include <algorithm>

namespace dl_turbo {

// IfbAddressCalculator Implementation
IfbAddressCalculator::IfbAddressCalculator(int width, int height, int bpp) {
    ResetGeometry(width, height, bpp);
}

void IfbAddressCalculator::ResetGeometry(int width, int height, int bpp) {
    m_width = width;
    m_height = height;
    m_bpp = bpp;
    m_stride = width * (bpp / 8);
    m_total_size = static_cast<size_t>(m_stride) * height;
}

size_t IfbAddressCalculator::ComputeTileOffset(int tile_x, int tile_y) const {
    return static_cast<size_t>(tile_y) * m_stride + (static_cast<size_t>(tile_x) * (m_bpp / 8));
}

// CfbBitBudget Implementation
CfbBitBudget::CfbBitBudget(uint32_t max_bits_per_pixel)
    : m_max_bpp(max_bits_per_pixel) {}

bool CfbBitBudget::CheckBudget(size_t compressed_bytes, int pixel_count) const {
    if (pixel_count <= 0) return true;
    uint64_t total_bits = static_cast<uint64_t>(compressed_bytes) * 8;
    uint64_t max_allowed_bits = static_cast<uint64_t>(pixel_count) * m_max_bpp;
    return total_bits <= max_allowed_bits;
}

// Dl3Encoder Implementation
Dl3Encoder::Dl3Encoder()
    : m_ifb_calc(1920, 1080),
      m_cfb_budget(16) {
    m_temp_buf.resize(64 * 1024);
}

size_t Dl3Encoder::CompressRle(
    const uint8_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_capacity
) {
    if (src_len == 0 || dst_capacity < 4) return 0;

    size_t src_idx = 0;
    size_t dst_idx = 0;

    while (src_idx < src_len && dst_idx + 3 < dst_capacity) {
        uint8_t byte = src[src_idx];
        uint8_t count = 1;

        while (src_idx + count < src_len && src[src_idx + count] == byte && count < 255) {
            count++;
        }

        if (count > 3 || byte == 0x55) { // 0x55 is DL escape byte
            dst[dst_idx++] = 0x55;
            dst[dst_idx++] = count;
            dst[dst_idx++] = byte;
            src_idx += count;
        } else {
            dst[dst_idx++] = byte;
            src_idx++;
        }
    }

    return dst_idx;
}

bool Dl3Encoder::EncodeTile(
    const uint8_t* raw_tile,
    const TileCoordinate& coord,
    std::vector<uint8_t>& out_packet,
    bool force_lossless
) {
    size_t raw_bytes = static_cast<size_t>(coord.width) * coord.height * 4;
    if (raw_bytes == 0) return false;

    if (m_temp_buf.size() < raw_bytes * 2) {
        m_temp_buf.resize(raw_bytes * 2);
    }

    // Attempt RLE compression
    size_t compressed_len = CompressRle(raw_tile, raw_bytes, m_temp_buf.data(), m_temp_buf.size());

    protocol::TileDescriptorHeader header;
    header.x = coord.x;
    header.y = coord.y;
    header.width = coord.width;
    header.height = coord.height;
    header.flags = force_lossless ? 0x01 : 0x00;

    // Check if compression saved significant bandwidth
    if (compressed_len > 0 && compressed_len < (raw_bytes * 85 / 100)) {
        header.compression = static_cast<uint8_t>(protocol::TileCompression::RunLengthDL1);
        header.compressed_bytes = static_cast<uint32_t>(compressed_len);

        size_t packet_size = sizeof(header) + compressed_len;
        out_packet.resize(packet_size);
        std::memcpy(out_packet.data(), &header, sizeof(header));
        std::memcpy(out_packet.data() + sizeof(header), m_temp_buf.data(), compressed_len);
    } else {
        // Fallback to Raw Uncompressed if entropy is high
        header.compression = static_cast<uint8_t>(protocol::TileCompression::RawUncompressed);
        header.compressed_bytes = static_cast<uint32_t>(raw_bytes);

        size_t packet_size = sizeof(header) + raw_bytes;
        out_packet.resize(packet_size);
        std::memcpy(out_packet.data(), &header, sizeof(header));
        std::memcpy(out_packet.data() + sizeof(header), raw_tile, raw_bytes);
    }

    return true;
}

} // namespace dl_turbo
