#ifndef VELOX_FORMAT_H
#define VELOX_FORMAT_H

#include "VeloxArch.h"
#include <cstring>

class FormatHandler {
public:
    static void SplitFloat32(const uint8_t* raw_bytes, size_t count, 
                             std::vector<velox_sample_t>& out_mantissa, 
                             std::vector<uint8_t>& out_exponent) {
        out_mantissa.resize(count);
        out_exponent.resize(count);
        const uint32_t* f32_ptr = (const uint32_t*)raw_bytes;
        
        for (size_t i = 0; i < count; i++) {
            uint32_t u = f32_ptr[i];
            uint32_t sign = (u >> 31);
            int32_t exp   = ((u >> 23) & 0xFF);
            uint32_t mant = (u & 0x7FFFFF);
            
            out_exponent[i] = (uint8_t)exp;
            
            if (exp != 0) mant |= 0x800000;
            
            if (sign) out_mantissa[i] = -(int64_t)mant;
            else      out_mantissa[i] = (int64_t)mant;
        }
    }

    static void MergeFloat32(const std::vector<velox_sample_t>& in_mantissa, 
                             const std::vector<uint8_t>& in_exponent, 
                             std::vector<uint8_t>& out_bytes) {
        size_t count = in_mantissa.size();
        out_bytes.resize(count * 4);
        uint32_t* f32_ptr = (uint32_t*)out_bytes.data();

        for (size_t i = 0; i < count; i++) {
            velox_sample_t m_val = in_mantissa[i];
            uint8_t exp = in_exponent[i];
            uint32_t sign = 0;

            if (m_val < 0) { sign = 1; m_val = -m_val; }
            
            uint32_t mant = (uint32_t)(m_val & 0x7FFFFF); 
            
            uint32_t u = (sign << 31) | ((uint32_t)exp << 23) | mant;
            f32_ptr[i] = u;
        }
    }

    static void BytesToSamples(const uint8_t* bytes, size_t count, int bits, std::vector<velox_sample_t>& out) {
        out.resize(count);
        size_t idx = 0;
        int bytes_per_sample = bits / 8;

        for(size_t i=0; i<count; i++) {
            if (bits == 16) {
                int16_t v; memcpy(&v, &bytes[idx], 2); out[i] = v;
            } else if (bits == 24) {
                uint32_t u = (uint32_t)bytes[idx] | ((uint32_t)bytes[idx+1] << 8) | ((uint32_t)bytes[idx+2] << 16);
                if (u & 0x800000) u |= 0xFF000000;
                out[i] = (int32_t)u;
            } else if (bits == 32) {
                int32_t v; memcpy(&v, &bytes[idx], 4); out[i] = v;
            }
            idx += bytes_per_sample;
        }
    }

    static void SamplesToBytes(const std::vector<velox_sample_t>& in, int bits, std::vector<uint8_t>& bytes) {
        int bytes_per_sample = bits / 8;
        size_t current_size = bytes.size();
        bytes.resize(current_size + in.size() * bytes_per_sample);
        uint8_t* ptr = bytes.data() + current_size;

        for(auto s : in) {
            if (bits == 16) {
                int16_t v = (int16_t)s; 
                memcpy(ptr, &v, 2); ptr += 2;
            } else if (bits == 24) {
                int32_t v = (int32_t)s;
                ptr[0] = v & 0xFF;
                ptr[1] = (v >> 8) & 0xFF;
                ptr[2] = (v >> 16) & 0xFF;
                ptr += 3;
            } else if (bits == 32) {
                int32_t v = (int32_t)s; 
                memcpy(ptr, &v, 4); ptr += 4;
            }
        }
    }
};

class LSBShifter {
public:
    static int Analyze(std::vector<velox_sample_t>& block) {
        if (block.empty()) return 0;
        uint64_t mask = 0;
        for(auto x : block) mask |= (uint64_t)std::abs(x);
        if (mask == 0) return 0;
        
        int shift = 0;
        while ((mask & 1) == 0 && shift < 32) {
            mask >>= 1;
            shift++;
        }
        return shift;
    }
    static void Apply(std::vector<velox_sample_t>& block, int shift) {
        if (shift <= 0) return;
        for(auto& x : block) x >>= shift;
    }
    static void Restore(std::vector<velox_sample_t>& block, int shift) {
        if (shift <= 0) return;
        for(auto& x : block) x <<= shift;
    }
};
#endif