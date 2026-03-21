#ifndef VELOX_ENTROPY_H
#define VELOX_ENTROPY_H

#include <vector>
#include <cstdint>
#include <cstring>

// --- BITSTREAM WRITER (64-BIT UPGRADE) ---
class BitStreamWriter
{
    std::vector<uint8_t> buffer;
    uint64_t bit_acc = 0;
    int bit_cnt = 0;

public:
    BitStreamWriter() { buffer.reserve(4 * 1024 * 1024); }

    inline void WriteBit(int bit)
    {
        if (bit)
            bit_acc |= (1ULL << bit_cnt);
        bit_cnt++;
        if (bit_cnt == 8)
        {
            buffer.push_back((uint8_t)bit_acc);
            bit_acc = 0;
            bit_cnt = 0;
        }
    }

    inline void Write(uint64_t val, int n)
    {
        for (int i = 0; i < n; i++)
            WriteBit((val >> i) & 1);
    }

    inline void Flush()
    {
        if (bit_cnt > 0)
            buffer.push_back((uint8_t)bit_acc);
    }
    const std::vector<uint8_t> &GetData() const { return buffer; }
};

// --- BITSTREAM READER ---
class BitStreamReader
{
    const uint8_t *data;
    size_t size;
    size_t pos = 0;
    uint64_t bit_acc = 0;
    int bit_cnt = 0;

public:
    BitStreamReader(const uint8_t *d, size_t s) : data(d), size(s) {}

    inline int ReadBit()
    {
        if (bit_cnt == 0)
        {
            if (pos >= size)
                return 0;
            bit_acc = data[pos++];
            bit_cnt = 8;
        }
        int val = bit_acc & 1;
        bit_acc >>= 1;
        bit_cnt--;
        return val;
    }

    // Change uint32_t to uint64_t
    inline uint64_t Read(int n)
    {
        uint64_t val = 0;
        for (int i = 0; i < n; i++)
            if (ReadBit())
                val |= (1ULL << i);
        return val;
    }

    inline int64_t ReadS(int n)
    {
        uint64_t v = Read(n);
        if (v & (1ULL << (n - 1)))
            return (int64_t)(v - (1ULL << n));
        return (int64_t)v;
    }
};

// --- ENTROPY UTILS (64-BIT) ---
class VeloxEntropy
{
public:
    static inline uint64_t ZigZag(int64_t n) { return (uint64_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint64_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    static void EncodeSample(BitStreamWriter &bs, int64_t val, int k)
    {
        uint64_t m = ZigZag(val);
        uint64_t q = m >> k;
        uint64_t r = m & ((1ULL << k) - 1);

        if (q < 64)
        {
            for (uint64_t i = 0; i < q; i++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            if (k > 0)
                bs.Write(r, k);
        }
        else
        {
            for (uint64_t i = 0; i < 64; i++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            bs.Write(m, 40);
        }
    }

    static int64_t DecodeSample(BitStreamReader &bs, int k)
    {
        uint64_t q = 0;
        while (bs.ReadBit())
            q++;

        uint64_t m;
        if (q < 64)
        {
            uint64_t r = (k > 0) ? bs.Read(k) : 0;
            m = (q << k) | r;
        }
        else
        {
            m = bs.Read(40);
        }
        return DeZigZag(m);
    }
};

#endif