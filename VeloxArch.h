#ifndef VELOX_ARCH_H
#define VELOX_ARCH_H

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

typedef int64_t velox_sample_t; 

// Cấu trúc File Header
struct VeloxHeader {
    uint32_t magic;         // VELX
    uint16_t version;       // 0x0200
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample; 
    uint16_t format_code;   // 1=PCM, 3=Float
    uint64_t total_samples;
    uint32_t header_blob_size; // Kích thước header gốc của file WAV
    uint32_t footer_blob_size; // Kích thước footer gốc
};

// Toán học số nguyên cho AI (Fixed Point Q20.12)
#define FX_SHIFT 12
#define FX_ONE   (1 << FX_SHIFT)

static inline int32_t FloatToFix(float f) { return (int32_t)(f * FX_ONE); }
static inline int32_t MulFix(int32_t a, int32_t b) { 
    return (int32_t)(((int64_t)a * b) >> FX_SHIFT); 
}
static inline int32_t SigmoidFix(int32_t x) {
    if (x > 4 * FX_ONE) return FX_ONE;
    if (x < -4 * FX_ONE) return 0;
    return (FX_ONE / 2) + (x >> 3); 
}

#endif