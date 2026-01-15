#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"

// --- BITSTREAM (Optimized) ---
class BitStream {
    std::vector<uint8_t> buffer;
    uint64_t bit_acc = 0; int bit_cnt = 0; size_t r_pos = 0;
public:
    BitStream() { buffer.reserve(8*1024*1024); }
    BitStream(const uint8_t* d, size_t s) : buffer(d, d+s) {}
    
    inline void Write(uint32_t v, int n) {
        for(int i=0;i<n;i++) {
            if((v>>i)&1) bit_acc |= (1ULL<<bit_cnt);
            bit_cnt++;
            if(bit_cnt==8) { buffer.push_back((uint8_t)bit_acc); bit_acc=0; bit_cnt=0; }
        }
    }
    inline void Flush() { if(bit_cnt>0) buffer.push_back((uint8_t)bit_acc); }
    const std::vector<uint8_t>& GetData() const { return buffer; }
    
    inline uint32_t Read(int n) {
        uint32_t v=0;
        for(int i=0;i<n;i++) {
            if(bit_cnt==0) { 
                if(r_pos>=buffer.size()) return v; 
                bit_acc=buffer[r_pos++]; bit_cnt=8; 
            }
            if(bit_acc&1) v|=(1<<i);
            bit_acc>>=1; bit_cnt--;
        }
        return v;
    }
    inline int32_t ReadS(int n) {
        uint32_t v = Read(n);
        if(v & (1<<(n-1))) return (int32_t)(v - (1<<n));
        return (int32_t)v;
    }
};

// --- NEURAL PREDICTOR (DUAL-GEAR) ---
class NeuralPredictor {
    static const int ORDER = 12; // Tăng order lên 12
    int32_t weights[ORDER];
    int32_t history[ORDER];
public:
    NeuralPredictor() { memset(weights,0,sizeof(weights)); memset(history,0,sizeof(history)); }
    
    inline int32_t Predict() {
        int64_t sum = 0;
        for(int i=0; i<ORDER; i++) sum += (int64_t)history[i] * weights[i];
        return (int32_t)(sum >> 11); // Q11 Fixed point (2048 = 1.0)
    }
    
    inline void Update(int32_t actual, int32_t pred) {
        int32_t err = actual - pred;
        int sign = (err > 0) ? 1 : ((err < 0) ? -1 : 0);
        if(sign == 0) return; // Perfect match
        
        // Adaptive Learning Rate:
        // Nếu lỗi lớn (>256), học nhanh (delta = 8)
        // Nếu lỗi nhỏ, học chậm (delta = 2)
        int delta = (std::abs(err) > 256) ? 8 : 2;

        for(int i=0; i<ORDER; i++) {
            int h_sign = (history[i]>0)?1:((history[i]<0)?-1:0);
            if(sign == h_sign) weights[i] += delta;
            else if(h_sign != 0) weights[i] -= delta;
            
            // Leakage (Giảm nhẹ weight để tránh bão hòa)
            if((i & 7) == 0) { // Mỗi 8 lần update thì giảm weight 1 chút
                 if(weights[i] > 0) weights[i]--;
                 if(weights[i] < 0) weights[i]++;
            }
        }
        
        for(int i=ORDER-1; i>0; i--) history[i] = history[i-1];
        history[0] = actual;
    }
};

class VeloxCodec {
    static inline uint32_t ZigZag(int64_t n) { return (uint32_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint32_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    // RLE cho Exponent (Float)
    static void EncodeRLE(const std::vector<uint8_t>& data, BitStream& bs) {
        if(data.empty()) return;
        uint8_t last = data[0];
        int run = 0;
        for(size_t i=0; i<data.size(); i++) {
            if(data[i] == last && run < 255) { run++; }
            else {
                bs.Write(run, 8); bs.Write(last, 8);
                last = data[i]; run = 1;
            }
        }
        bs.Write(run, 8); bs.Write(last, 8);
    }

    static std::vector<uint8_t> DecodeRLE(BitStream& bs, size_t count) {
        std::vector<uint8_t> out; out.reserve(count);
        while(out.size() < count) {
            int run = bs.Read(8);
            int val = bs.Read(8);
            for(int i=0; i<run; i++) out.push_back(val);
        }
        return out;
    }

    // Pipeline Encode
    static void EncodeStream(std::vector<velox_sample_t>& data, BitStream& bs) {
        // 1. LSB Shift (Giảm bit nhiễu)
        int shift = LSBShifter::Analyze(data);
        LSBShifter::Apply(data, shift);
        bs.Write(shift, 5);

        // 2. Neural + Context Rice
        NeuralPredictor ai;
        uint32_t run_avg = 512; 

        for(auto val : data) {
            int32_t pred = ai.Predict();
            int32_t resid = (int32_t)val - pred;
            
            // Context K calculation
            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }
            
            uint32_t m = ZigZag(resid);
            uint32_t q = m >> k;
            uint32_t r = m & ((1<<k)-1);

            if(q < 32) { // Standard Rice
                for(uint32_t i=0;i<q;i++) bs.Write(1,1);
                bs.Write(0,1);
                if(k>0) bs.Write(r, k);
            } else { // Escape
                for(uint32_t i=0;i<32;i++) bs.Write(1,1);
                bs.Write(0,1);
                bs.Write(m, 32); 
            }

            ai.Update((int32_t)val, pred);
            run_avg = run_avg - (run_avg>>3) + (m>>3);
            if(run_avg < 1) run_avg = 1;
        }
    }

    static void DecodeStream(BitStream& bs, size_t count, std::vector<velox_sample_t>& out) {
        int shift = bs.Read(5);
        out.resize(count);
        
        NeuralPredictor ai;
        uint32_t run_avg = 512;

        for(size_t i=0; i<count; i++) {
            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }

            uint32_t q = 0;
            while(bs.Read(1)) q++;
            
            uint32_t m;
            if(q < 32) {
                uint32_t r = (k>0) ? bs.Read(k) : 0;
                m = (q<<k)|r;
            } else {
                m = bs.Read(32);
            }

            int32_t resid = (int32_t)DeZigZag(m);
            int32_t pred = ai.Predict();
            int32_t val = resid + pred;
            
            out[i] = val;
            ai.Update(val, pred);
            run_avg = run_avg - (run_avg>>3) + (m>>3);
            if(run_avg < 1) run_avg = 1;
        }
        LSBShifter::Restore(out, shift);
    }

public:
    static std::vector<uint8_t> EncodeBlock(const std::vector<velox_sample_t>& samples, bool is_float, const std::vector<uint8_t>& exps) {
        BitStream bs;
        bs.Write(is_float, 1);
        if(is_float) EncodeRLE(exps, bs);
        
        std::vector<velox_sample_t> work = samples;
        EncodeStream(work, bs);
        
        bs.Flush();
        return bs.GetData();
    }

    static void DecodeBlock(const uint8_t* data, size_t size, size_t count, 
                            std::vector<velox_sample_t>& out_samps, 
                            std::vector<uint8_t>& out_exps, bool& is_float) {
        BitStream bs(data, size);
        is_float = bs.Read(1);
        if(is_float) out_exps = DecodeRLE(bs, count);
        DecodeStream(bs, count, out_samps);
    }
};
#endif