#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"
#include <numeric>

// --- BITSTREAM ---
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

// --- NEURAL RESIDUAL CORRECTOR ---
class NeuralCorrector {
    static const int ORDER = 4; 
    int32_t weights[ORDER];
    int32_t history[ORDER];
public:
    NeuralCorrector() { memset(weights,0,sizeof(weights)); memset(history,0,sizeof(history)); }
    
    inline int32_t Predict() {
        int64_t sum = 0;
        for(int i=0; i<ORDER; i++) sum += (int64_t)history[i] * weights[i];
        return (int32_t)(sum >> 9); 
    }
    
    inline void Update(int32_t actual, int32_t pred) {
        int32_t err = actual - pred;
        int sign = (err > 0) ? 1 : ((err < 0) ? -1 : 0);
        if(sign == 0) return;
        
        for(int i=0; i<ORDER; i++) {
            int h_sign = (history[i]>0)?1:((history[i]<0)?-1:0);
            if(sign == h_sign) weights[i] += 8;     
            else if(h_sign != 0) weights[i] -= 8;
        }
        for(int i=ORDER-1; i>0; i--) history[i] = history[i-1];
        history[0] = actual;
    }
};

// --- CORE CODEC ---
class VeloxCodec {
    static inline uint32_t ZigZag(int64_t n) { return (uint32_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint32_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    static void ComputeLPC(const std::vector<velox_sample_t>& data, int order, std::vector<int>& coeffs, int& shift) {
        if(data.empty()) return;
        
        double autocorr[13]; 
        int stride = 1; 
        if(data.size() > 4096) stride = 2; 
        
        for (int i = 0; i <= order; ++i) {
            double sum = 0;
            for (size_t j = i; j < data.size(); j+=stride) sum += (double)data[j] * data[j - i];
            autocorr[i] = sum;
        }

        if(std::abs(autocorr[0]) < 1e-9) { shift=0; coeffs.assign(order, 0); return; }

        double a[13][13] = {0};
        double e[13] = {0};
        e[0] = autocorr[0];

        for (int i = 1; i <= order; ++i) {
            double k = autocorr[i];
            for (int j = 1; j < i; ++j) k -= a[j][i - 1] * autocorr[i - j];
            k /= e[i - 1];
            if(k > 0.999) k = 0.999; if(k < -0.999) k = -0.999; 

            a[i][i] = k;
            for (int j = 1; j < i; ++j) a[j][i] = a[j][i - 1] - k * a[i - j][i - 1];
            e[i] = e[i - 1] * (1 - k * k);
        }

        shift = 11; 
        coeffs.resize(order);
        for (int i = 1; i <= order; ++i) 
            coeffs[i-1] = (int)std::floor(a[i][order] * (1 << shift) + 0.5);
    }

    // --- ENCODE CHANNEL ---
    static void EncodeChannel(std::vector<velox_sample_t>& data, BitStream& bs) {
        int shift_lsb = LSBShifter::Analyze(data);
        LSBShifter::Apply(data, shift_lsb);
        bs.Write(shift_lsb, 5);

        int order = 8; 
        int lpc_shift = 0;
        std::vector<int> lpc_coeffs;
        ComputeLPC(data, order, lpc_coeffs, lpc_shift);
        
        bs.Write(lpc_shift, 5);
        for(int c : lpc_coeffs) bs.Write(c & 0xFFFF, 16);

        NeuralCorrector neural;
        uint32_t run_avg = 512;

        std::vector<velox_sample_t> buffer(order, 0); 
        
        for(size_t i=0; i<data.size(); i++) {
            velox_sample_t original = data[i];

            int64_t sum = 0;
            for(int j=0; j<order; j++) {
                if(i > (size_t)j) sum += (int64_t)lpc_coeffs[j] * buffer[(i-1-j) % order]; 
            }
            sum = 0;
            for(int j=0; j<order; j++) {
                if(i > (size_t)j) sum += (int64_t)lpc_coeffs[j] * data[i-1-j];
            }
            
            int32_t predLPC = (int32_t)(sum >> lpc_shift);
            int32_t resLPC = (int32_t)original - predLPC;

            int32_t predNeural = neural.Predict();
            int32_t finalRes = resLPC - predNeural;

            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }
            
            uint32_t m = ZigZag(finalRes);
            uint32_t q = m >> k;
            uint32_t r = m & ((1<<k)-1);

            if(q < 32) {
                for(uint32_t x=0;x<q;x++) bs.Write(1,1);
                bs.Write(0,1);
                if(k>0) bs.Write(r, k);
            } else {
                for(uint32_t x=0;x<32;x++) bs.Write(1,1);
                bs.Write(0,1);
                bs.Write(m, 32); 
            }

            // D. Update States
            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg>>3) + (m>>3);
            if(run_avg < 1) run_avg = 1;
        }
    }

    // --- DECODE CHANNEL ---
    static void DecodeChannel(BitStream& bs, size_t count, std::vector<velox_sample_t>& out) {
        int shift_lsb = bs.Read(5);
        
        int order = 8;
        int lpc_shift = bs.Read(5);
        std::vector<int> lpc_coeffs(order);
        for(int i=0; i<order; i++) lpc_coeffs[i] = bs.ReadS(16);

        NeuralCorrector neural;
        uint32_t run_avg = 512;
        
        out.resize(count);

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

            int32_t finalRes = (int32_t)DeZigZag(m);
            int32_t predNeural = neural.Predict();
            int32_t resLPC = finalRes + predNeural;

            int64_t sum = 0;
            for(int j=0; j<order; j++) {
                if(i > (size_t)j) sum += (int64_t)lpc_coeffs[j] * out[i-1-j];
            }
            int32_t predLPC = (int32_t)(sum >> lpc_shift);
            
            int32_t val = resLPC + predLPC;
            out[i] = val;

            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg>>3) + (m>>3);
            if(run_avg < 1) run_avg = 1;
        }

        LSBShifter::Restore(out, shift_lsb);
    }

    // Helper functions
    static void EncodeRLE(const std::vector<uint8_t>& data, BitStream& bs) {
        if(data.empty()) return;
        uint8_t last = data[0];
        int run = 0;
        for(size_t i=0; i<data.size(); i++) {
            if(data[i] == last && run < 255) { run++; }
            else { bs.Write(run, 8); bs.Write(last, 8); last = data[i]; run = 1; }
        }
        bs.Write(run, 8); bs.Write(last, 8);
    }

    static std::vector<uint8_t> DecodeRLE(BitStream& bs, size_t count) {
        std::vector<uint8_t> out; out.reserve(count);
        while(out.size() < count) {
            int run = bs.Read(8); int val = bs.Read(8);
            for(int i=0; i<run; i++) out.push_back(val);
        }
        return out;
    }

public:
    static std::vector<uint8_t> EncodeBlock(const std::vector<velox_sample_t>& samples, bool is_float, const std::vector<uint8_t>& exps) {
        BitStream bs;
        bs.Write(is_float, 1);
        if(is_float) EncodeRLE(exps, bs);
        
        // --- ADAPTIVE STEREO ---
        const size_t SUB_BLOCK = 4096;
        size_t total = samples.size();
        
        if(total % 2 != 0) { 
            std::vector<velox_sample_t> mono = samples; 
            EncodeChannel(mono, bs); 
            bs.Flush(); return bs.GetData(); 
        }

        for(size_t i=0; i<total; i += SUB_BLOCK) {
            size_t end = std::min(i + SUB_BLOCK, total);
            if((end - i) % 2 != 0) end--; 
            
            size_t len = (end - i) / 2;
            
            std::vector<velox_sample_t> chL, chR;
            chL.reserve(len); chR.reserve(len);
            
            uint64_t sad_LR = 0;
            uint64_t sad_MS = 0;

            for(size_t j=0; j<len; j++) {
                velox_sample_t L = samples[i + j*2];
                velox_sample_t R = samples[i + j*2 + 1];
                chL.push_back(L);
                chR.push_back(R);
                
                sad_LR += std::abs(L) + std::abs(R);
                sad_MS += std::abs((L+R)>>1) + std::abs(L-R);
            }

            bool use_MS = (sad_MS < sad_LR);
            bs.Write(use_MS, 1);

            if(use_MS) {
                // Transform inplace
                for(size_t j=0; j<len; j++) {
                    velox_sample_t L = chL[j];
                    velox_sample_t R = chR[j];
                    chL[j] = (L+R)>>1;
                    chR[j] = L-R;
                }
            }
            
            EncodeChannel(chL, bs);
            EncodeChannel(chR, bs);
        }
        
        bs.Flush();
        return bs.GetData();
    }

    static void DecodeBlock(const uint8_t* data, size_t size, size_t count, 
                            std::vector<velox_sample_t>& out_samps, 
                            std::vector<uint8_t>& out_exps, bool& is_float) {
        BitStream bs(data, size);
        is_float = bs.Read(1);
        if(is_float) out_exps = DecodeRLE(bs, count);
        
        out_samps.resize(count);
        const size_t SUB_BLOCK = 4096;
        
        if(count % 2 != 0) {
            DecodeChannel(bs, count, out_samps);
            return;
        }

        size_t current = 0;
        while(current < count) {
            size_t end = std::min(current + SUB_BLOCK, count);
            if((end - current) % 2 != 0) end--;
            size_t len = (end - current) / 2;

            int use_MS = bs.Read(1);
            
            std::vector<velox_sample_t> c1, c2;
            DecodeChannel(bs, len, c1);
            DecodeChannel(bs, len, c2);

            for(size_t j=0; j<len; j++) {
                velox_sample_t v1 = c1[j];
                velox_sample_t v2 = c2[j];
                velox_sample_t L, R;
                
                if(use_MS) {
                    L = v1 + ((v2+1)>>1);
                    R = v1 - (v2>>1);
                } else {
                    L = v1; R = v2;
                }
                
                out_samps[current + j*2] = L;
                out_samps[current + j*2 + 1] = R;
            }
            current = end;
        }
    }
};

#endif