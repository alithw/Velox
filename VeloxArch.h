#ifndef VELOX_ARCH_H
#define VELOX_ARCH_H

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

typedef int64_t velox_sample_t;

#pragma pack(push, 1)
struct VeloxHeader
{
    uint32_t magic;
    uint16_t version;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t format_code;
    uint64_t total_samples;
    uint32_t header_blob_size;
    uint32_t footer_blob_size;
    uint32_t seek_table_offset; // Seek table start position
    uint32_t seek_table_count;  // Number of seek points
};

struct VeloxSeekPoint
{
    uint64_t sample_offset; // Sample index (e.g., 48000, 96000...)
    uint64_t byte_offset;   // Byte offset in file
};
#pragma pack(pop)

// Fixed Point Math
#define FX_SHIFT 12
#define FX_ONE (1 << FX_SHIFT)
static inline int32_t FloatToFix(float f) { return (int32_t)(f * FX_ONE); }
static inline int32_t MulFix(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> FX_SHIFT); }
static inline int32_t SigmoidFix(int32_t x)
{
    if (x > 4 * FX_ONE)
        return FX_ONE;
    if (x < -4 * FX_ONE)
        return 0;
    return (FX_ONE / 2) + (x >> 3);
}

#endif