#ifndef VELOX_ENTROPY_H
#define VELOX_ENTROPY_H

#include <vector>
#include <cstdint>
#include <cstring>

// --- BITSTREAM READER/WRITER TỐI ƯU ---
class BitStreamWriter {
    std::vector<uint8_t> buffer;
    uint64_t bit_acc = 0; int bit_cnt = 0;
public:
    BitStreamWriter() { buffer.reserve(4 * 1024 * 1024); }
    
    inline void WriteBit(int bit) {
        if (bit) bit_acc |= (1ULL << bit_cnt);
        bit_cnt++;
        if (bit_cnt == 8) {
            buffer.push_back((uint8_t)bit_acc);
            bit_acc = 0; bit_cnt = 0;
        }
    }
    
    inline void Write(uint32_t val, int n) {
        for(int i=0; i<n; i++) WriteBit((val >> i) & 1);
    }
    
    inline void Flush() { if(bit_cnt > 0) buffer.push_back((uint8_t)bit_acc); }
    const std::vector<uint8_t>& GetData() const { return buffer; }
};

class BitStreamReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint64_t bit_acc = 0; int bit_cnt = 0;
public:
    BitStreamReader(const uint8_t* d, size_t s) : data(d), size(s) {}
    
    inline int ReadBit() {
        if (bit_cnt == 0) {
            if (pos >= size) return 0;
            bit_acc = data[pos++];
            bit_cnt = 8;
        }
        int val = bit_acc & 1;
        bit_acc >>= 1;
        bit_cnt--;
        return val;
    }
    
    inline uint32_t Read(int n) {
        uint32_t val = 0;
        for(int i=0; i<n; i++) if(ReadBit()) val |= (1 << i);
        return val;
    }
    
    inline int32_t ReadS(int n) {
        uint32_t v = Read(n);
        if(v & (1 << (n-1))) return (int32_t)(v - (1 << n));
        return (int32_t)v;
    }
};

// --- ENTROPY CODER ---
class VeloxEntropy {
public:
    static inline uint32_t ZigZag(int64_t n) { return (uint32_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint32_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    static void EncodeSample(BitStreamWriter& bs, int32_t val, int k) {
        uint32_t m = ZigZag(val);
        uint32_t q = m >> k;
        uint32_t r = m & ((1 << k) - 1);

        if (q < 32) {
            for(uint32_t i=0; i<q; i++) bs.WriteBit(1);
            bs.WriteBit(0);
            if(k > 0) bs.Write(r, k);
        } else {
            for(uint32_t i=0; i<32; i++) bs.WriteBit(1);
            bs.WriteBit(0);
            bs.Write(m, 32);
        }
    }

    static int32_t DecodeSample(BitStreamReader& bs, int k) {
        uint32_t q = 0;
        while(bs.ReadBit()) q++;
        
        uint32_t m;
        if (q < 32) {
            uint32_t r = (k > 0) ? bs.Read(k) : 0;
            m = (q << k) | r;
        } else {
            m = bs.Read(32);
        }
        return (int32_t)DeZigZag(m);
    }
};

#endif