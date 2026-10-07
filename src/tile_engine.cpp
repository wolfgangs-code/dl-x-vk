#include "tile_engine.hpp"
#include <algorithm>
#include <cstring>
#include <immintrin.h>

namespace dl_turbo {

TileEngine::TileEngine(int screen_width, int screen_height, int tile_size)
    : m_screen_width(screen_width),
      m_screen_height(screen_height),
      m_tile_size(tile_size),
      m_grid_cols((screen_width + tile_size - 1) / tile_size),
      m_grid_rows((screen_height + tile_size - 1) / tile_size) {
    m_tile_hashes.assign(m_grid_cols * m_grid_rows, 0);
}

void TileEngine::Resize(int screen_width, int screen_height) {
    m_screen_width = screen_width;
    m_screen_height = screen_height;
    m_grid_cols = (screen_width + m_tile_size - 1) / m_tile_size;
    m_grid_rows = (screen_height + m_tile_size - 1) / m_tile_size;
    m_tile_hashes.assign(m_grid_cols * m_grid_rows, 0);
}

std::vector<DirtyRect> TileEngine::CoalesceRects(const std::vector<DirtyRect>& rects) {
    if (rects.empty()) return {};

    std::vector<DirtyRect> merged;
    for (const auto& r : rects) {
        if (r.x2 <= r.x1 || r.y2 <= r.y1) continue;

        bool fused = false;
        for (auto& m : merged) {
            // Check if rectangles intersect or touch
            if (!(r.x2 < m.x1 || r.x1 > m.x2 || r.y2 < m.y1 || r.y1 > m.y2)) {
                m.x1 = std::min(m.x1, r.x1);
                m.y1 = std::min(m.y1, r.y1);
                m.x2 = std::max(m.x2, r.x2);
                m.y2 = std::max(m.y2, r.y2);
                fused = true;
                break;
            }
        }
        if (!fused) {
            merged.push_back(r);
        }
    }
    return merged;
}

std::vector<TileCoordinate> TileEngine::GenerateDirtyTiles(const std::vector<DirtyRect>& dirty_rects) {
    std::vector<bool> dirty_grid(m_grid_cols * m_grid_rows, false);

    for (const auto& r : dirty_rects) {
        int col_start = std::max(0, r.x1 / m_tile_size);
        int col_end   = std::min(m_grid_cols - 1, (r.x2 - 1) / m_tile_size);
        int row_start = std::max(0, r.y1 / m_tile_size);
        int row_end   = std::min(m_grid_rows - 1, (r.y2 - 1) / m_tile_size);

        for (int row = row_start; row <= row_end; ++row) {
            for (int col = col_start; col <= col_end; ++col) {
                dirty_grid[row * m_grid_cols + col] = true;
            }
        }
    }

    std::vector<TileCoordinate> tiles;
    tiles.reserve(dirty_grid.size() / 4);

    for (int row = 0; row < m_grid_rows; ++row) {
        for (int col = 0; col < m_grid_cols; ++col) {
            if (dirty_grid[row * m_grid_cols + col]) {
                TileCoordinate t;
                t.x = static_cast<uint16_t>(col * m_tile_size);
                t.y = static_cast<uint16_t>(row * m_tile_size);
                t.width  = static_cast<uint16_t>(std::min(m_tile_size, m_screen_width - t.x));
                t.height = static_cast<uint16_t>(std::min(m_tile_size, m_screen_height - t.y));
                tiles.push_back(t);
            }
        }
    }

    return tiles;
}

void TileEngine::ExtractTileRgb32(
    const uint8_t* master_fb,
    int fb_stride,
    const TileCoordinate& tile,
    std::vector<uint8_t>& out_tile_buf
) const {
    size_t tile_bytes = static_cast<size_t>(tile.width) * tile.height * 4;
    out_tile_buf.resize(tile_bytes);

    for (int row = 0; row < tile.height; ++row) {
        const uint8_t* src_row = master_fb + (tile.y + row) * fb_stride + (tile.x * 4);
        uint8_t* dst_row = out_tile_buf.data() + (row * tile.width * 4);
        std::memcpy(dst_row, src_row, tile.width * 4);
    }
}

uint64_t TileEngine::ComputeTileHash(const uint8_t* master_fb, int fb_stride, const TileCoordinate& tile) const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int row = 0; row < tile.height; ++row) {
        const uint64_t* src_row = reinterpret_cast<const uint64_t*>(master_fb + (tile.y + row) * fb_stride + (tile.x * 4));
        size_t words = (tile.width * 4) / 8;
        for (size_t w = 0; w < words; ++w) {
#if defined(__SSE4_2__)
            h = _mm_crc32_u64(h, src_row[w]);
#else
            h ^= src_row[w];
            h *= 0x100000001b3ULL;
#endif
        }
    }
    return h;
}

std::vector<TileCoordinate> TileEngine::FilterChangedTiles(
    const uint8_t* master_fb,
    int fb_stride,
    const std::vector<TileCoordinate>& candidate_tiles
) {
    std::vector<TileCoordinate> changed;
    changed.reserve(candidate_tiles.size());

    for (const auto& tile : candidate_tiles) {
        int col = tile.x / m_tile_size;
        int row = tile.y / m_tile_size;
        size_t tile_idx = row * m_grid_cols + col;

        uint64_t current_hash = ComputeTileHash(master_fb, fb_stride, tile);
        if (tile_idx < m_tile_hashes.size()) {
            if (current_hash != m_tile_hashes[tile_idx]) {
                m_tile_hashes[tile_idx] = current_hash;
                changed.push_back(tile);
            }
        } else {
            changed.push_back(tile);
        }
    }

    return changed;
}

} // namespace dl_turbo
