#pragma once

#include <cstdint>
#include <cstddef>

namespace dl_turbo {

struct Yuv420Planes {
    uint8_t* y_plane;
    uint8_t* u_plane;
    uint8_t* v_plane;
    int y_stride;
    int uv_stride;
};

class ColorConverter {
public:
    ColorConverter();
    ~ColorConverter() = default;

    // Convert ARGB/XRGB8888 32bpp tile into planar YUV420 buffers
    static void Rgb32ToYuv420(
        const uint8_t* src_argb,
        int src_stride,
        int width,
        int height,
        uint8_t* dst_y,
        uint8_t* dst_u,
        uint8_t* dst_v,
        int dst_y_stride,
        int dst_uv_stride
    );

    // Decompiled DisplayLink BT.601 YUV420 to RGB32 kernel (scalar from 0x004ec3f0)
    static void Yuv420ToRgb32Scalar(
        const uint8_t* src_y,
        const uint8_t* src_u,
        const uint8_t* src_v,
        uint8_t* dst_rgba,
        int width,
        int count
    );

    // SIMD AVX2/SSSE3 vector path (decompiled from 0x004ea370)
    static void Yuv420ToRgb32Simd(
        const uint8_t* src_y,
        const uint8_t* src_u,
        const uint8_t* src_v,
        uint8_t* dst_rgba,
        int width,
        int count
    );

    // Fast SIMD-accelerated RGB to YUV420 converter
    static void Rgb32ToYuv420Avx2(
        const uint8_t* src_argb,
        int src_stride,
        int width,
        int height,
        uint8_t* dst_y,
        uint8_t* dst_u,
        uint8_t* dst_v,
        int dst_y_stride,
        int dst_uv_stride
    );

private:
    static bool s_has_avx2;
    static bool DetectAvx2();
};

} // namespace dl_turbo
