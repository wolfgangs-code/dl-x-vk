#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "protocol.hpp"

namespace dl_turbo {

struct TileCoordinate {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

struct DirtyRect {
    int x1;
    int y1;
    int x2;
    int y2;
};

class TileEngine {
public:
    static constexpr int DEFAULT_TILE_SIZE = 32;

    TileEngine(int screen_width, int screen_height, int tile_size = DEFAULT_TILE_SIZE);
    ~TileEngine() = default;

    // Set or update screen resolution
    void Resize(int screen_width, int screen_height);

    // Ingest EVDI damaged rectangles and compute tile-aligned dirty list
    std::vector<TileCoordinate> GenerateDirtyTiles(const std::vector<DirtyRect>& dirty_rects);

    // Coalesce overlapping or adjacent dirty rectangles into minimal bounding boxes
    static std::vector<DirtyRect> CoalesceRects(const std::vector<DirtyRect>& rects);

    // Extract raw pixels for a specific tile from the master frame buffer
    void ExtractTileRgb32(
        const uint8_t* master_fb,
        int fb_stride,
        const TileCoordinate& tile,
        std::vector<uint8_t>& out_tile_buf
    ) const;

    // Filter dirty tiles using 64-bit SIMD/CRC32 temporal differencing against cached frame
    std::vector<TileCoordinate> FilterChangedTiles(
        const uint8_t* master_fb,
        int fb_stride,
        const std::vector<TileCoordinate>& candidate_tiles
    );

    int GetTileSize() const { return m_tile_size; }
    int GetGridCols() const { return m_grid_cols; }
    int GetGridRows() const { return m_grid_rows; }

private:
    uint64_t ComputeTileHash(const uint8_t* master_fb, int fb_stride, const TileCoordinate& tile) const;

    int m_screen_width;
    int m_screen_height;
    int m_tile_size;
    int m_grid_cols;
    int m_grid_rows;
    std::vector<uint64_t> m_tile_hashes;
};

} // namespace dl_turbo
