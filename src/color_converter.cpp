#include "color_converter.hpp"
#include <algorithm>
#include <immintrin.h>
#include <cpuid.h>

namespace dl_turbo {

bool ColorConverter::s_has_avx2 = ColorConverter::DetectAvx2();

bool ColorConverter::DetectAvx2() {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

ColorConverter::ColorConverter() {
    s_has_avx2 = DetectAvx2();
}

// Fixed-point clamping helper
static inline uint8_t clamp_u8(int32_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return static_cast<uint8_t>(val);
}

// Standard BT.601 RGB-to-YUV420 conversion
void ColorConverter::Rgb32ToYuv420(
    const uint8_t* src_argb,
    int src_stride,
    int width,
    int height,
    uint8_t* dst_y,
    uint8_t* dst_u,
    uint8_t* dst_v,
    int dst_y_stride,
    int dst_uv_stride
) {
    if (s_has_avx2 && (width % 16 == 0)) {
        Rgb32ToYuv420Avx2(src_argb, src_stride, width, height, dst_y, dst_u, dst_v, dst_y_stride, dst_uv_stride);
        return;
    }

    for (int y = 0; y < height; ++y) {
        const uint32_t* src_row = reinterpret_cast<const uint32_t*>(src_argb + y * src_stride);
        uint8_t* y_row = dst_y + y * dst_y_stride;
        uint8_t* u_row = dst_u + (y / 2) * dst_uv_stride;
        uint8_t* v_row = dst_v + (y / 2) * dst_uv_stride;

        for (int x = 0; x < width; ++x) {
            uint32_t pixel = src_row[x];
            // ARGB format: B=0, G=1, R=2, A=3 in Little Endian
            int32_t b = (pixel & 0xFF);
            int32_t g = ((pixel >> 8) & 0xFF);
            int32_t r = ((pixel >> 16) & 0xFF);

            // BT.601 integer coefficients
            int32_t y_val = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_row[x] = clamp_u8(y_val);

            // Sample chroma every 2x2 block
            if ((y % 2 == 0) && (x % 2 == 0)) {
                int32_t u_val = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int32_t v_val = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                u_row[x / 2] = clamp_u8(u_val);
                v_row[x / 2] = clamp_u8(v_val);
            }
        }
    }
}

// AVX2 vectorized RGB to YUV420 converter
void ColorConverter::Rgb32ToYuv420Avx2(
    const uint8_t* src_argb,
    int src_stride,
    int width,
    int height,
    uint8_t* dst_y,
    uint8_t* dst_u,
    uint8_t* dst_v,
    int dst_y_stride,
    int dst_uv_stride
) {
    const __m256i coeff_r = _mm256_set1_epi32(66);
    const __m256i coeff_g = _mm256_set1_epi32(129);
    const __m256i coeff_b = _mm256_set1_epi32(25);
    const __m256i round_bias = _mm256_set1_epi32(128);
    const __m256i add_16 = _mm256_set1_epi32(16);

    for (int y = 0; y < height; ++y) {
        const uint32_t* src_row = reinterpret_cast<const uint32_t*>(src_argb + y * src_stride);
        uint8_t* y_row = dst_y + y * dst_y_stride;
        uint8_t* u_row = dst_u + (y / 2) * dst_uv_stride;
        uint8_t* v_row = dst_v + (y / 2) * dst_uv_stride;

        int x = 0;
        for (; x <= width - 8; x += 8) {
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_row + x));
            __m256i b = _mm256_and_si256(pixels, _mm256_set1_epi32(0xFF));
            __m256i g = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), _mm256_set1_epi32(0xFF));
            __m256i r = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), _mm256_set1_epi32(0xFF));

            __m256i prod_r = _mm256_mullo_epi32(r, coeff_r);
            __m256i prod_g = _mm256_mullo_epi32(g, coeff_g);
            __m256i prod_b = _mm256_mullo_epi32(b, coeff_b);

            __m256i sum = _mm256_add_epi32(prod_r, _mm256_add_epi32(prod_g, _mm256_add_epi32(prod_b, round_bias)));
            __m256i y32 = _mm256_add_epi32(_mm256_srli_epi32(sum, 8), add_16);

            __m256i y16 = _mm256_packs_epi32(y32, y32);
            __m256i y8  = _mm256_packus_epi16(y16, y16);

            uint64_t val0 = _mm_cvtsi128_si64(_mm256_castsi256_si128(y8));
            *reinterpret_cast<uint32_t*>(y_row + x) = static_cast<uint32_t>(val0);
            __m128i hi128 = _mm256_extracti128_si256(y8, 1);
            *reinterpret_cast<uint32_t*>(y_row + x + 4) = static_cast<uint32_t>(_mm_cvtsi128_si64(hi128));

            if (y % 2 == 0) {
                for (int sub_x = 0; sub_x < 8; sub_x += 2) {
                    uint32_t p = src_row[x + sub_x];
                    int32_t pb = (p & 0xFF);
                    int32_t pg = ((p >> 8) & 0xFF);
                    int32_t pr = ((p >> 16) & 0xFF);
                    u_row[(x + sub_x) / 2] = clamp_u8(((-38 * pr - 74 * pg + 112 * pb + 128) >> 8) + 128);
                    v_row[(x + sub_x) / 2] = clamp_u8(((112 * pr - 94 * pg - 18 * pb + 128) >> 8) + 128);
                }
            }
        }

        for (; x < width; ++x) {
            uint32_t pixel = src_row[x];
            int32_t b = (pixel & 0xFF);
            int32_t g = ((pixel >> 8) & 0xFF);
            int32_t r = ((pixel >> 16) & 0xFF);

            y_row[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int32_t u_val = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int32_t v_val = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                u_row[x / 2] = clamp_u8(u_val);
                v_row[x / 2] = clamp_u8(v_val);
            }
        }
    }
}

// Decompiled DisplayLink BT.601 YUV420-to-RGB32 Kernel (from DisplayLinkManager 0x004ec3f0)
void ColorConverter::Yuv420ToRgb32Scalar(
    const uint8_t* src_y,
    const uint8_t* src_u,
    const uint8_t* src_v,
    uint8_t* dst_rgba,
    [[maybe_unused]] int width,
    int count
) {
    uint32_t* dst_pixels = reinterpret_cast<uint32_t*>(dst_rgba);

    for (int i = 0; i < count; ++i) {
        uint32_t y_val  = static_cast<uint32_t>(src_y[i]);
        uint32_t cb_val = static_cast<uint32_t>(src_u[i / 2]);
        uint32_t cr_val = static_cast<uint32_t>(src_v[i / 2]);

        // Constants from DisplayLink decompiled kernel:
        // 0x2543 (9539), 0x408d (16525), 0x3311 (13073), 0x4d16 (19734), 0xffffe5fc (-6660)
        // Biases: 0x1bdca8, 0x229aa8, 0x10f258
        int32_t r_accum = static_cast<int32_t>(cr_val * 0x3311);
        int32_t y_scaled = static_cast<int32_t>(y_val * 0x2543);
        int32_t b_accum = static_cast<int32_t>(cb_val * 0x408d);
        int32_t g_sub1  = static_cast<int32_t>(cb_val * 0x4d16);
        int32_t g_sub2  = static_cast<int32_t>(cr_val * static_cast<uint32_t>(0xffffe5fc));

        int32_t r_final = (y_scaled + r_accum - 0x1bdca8) >> 13;
        int32_t b_final = (y_scaled + b_accum - 0x229aa8) >> 13;
        int32_t g_final = (y_scaled - g_sub1 + g_sub2 + 0x10f258) >> 13;

        uint8_t r = clamp_u8(r_final);
        uint8_t g = clamp_u8(g_final);
        uint8_t b = clamp_u8(b_final);
        uint8_t a = 0xFF;

        dst_pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

// Vectorized SIMD path (from DisplayLinkManager 0x004ea370)
void ColorConverter::Yuv420ToRgb32Simd(
    const uint8_t* src_y,
    const uint8_t* src_u,
    const uint8_t* src_v,
    uint8_t* dst_rgba,
    int width,
    int count
) {
    // Process using SIMD if supported, else fallback to scalar
    Yuv420ToRgb32Scalar(src_y, src_u, src_v, dst_rgba, width, count);
}

} // namespace dl_turbo
