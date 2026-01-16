#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"

// --- BITSTREAM (Reader) ---
class BitStreamReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint64_t bit_acc = 0;
    int bit_cnt = 0;
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
        uint32_t v = 0;
        for (int i = 0; i < n; i++) if (ReadBit()) v |= (1 << i);
        return v;
    }
    
    inline int32_t ReadS(int n) {
        uint32_t v = Read(n);
        if (v & (1 << (n - 1))) return (int32_t)(v - (1 << n));
        return (int32_t)v;
    }
    
    void Reset() { pos = 0; bit_acc = 0; bit_cnt = 0; }
};

// --- BITSTREAM WRITER (Giữ nguyên) ---
class BitStreamWriter {
    std::vector<uint8_t> buffer;
    uint64_t bit_acc = 0; int bit_cnt = 0;
public:
    BitStreamWriter() { buffer.reserve(8*1024*1024); }
    inline void Write(uint32_t v, int n) {
        for(int i=0;i<n;i++) {
            if((v>>i)&1) bit_acc |= (1ULL<<bit_cnt);
            bit_cnt++;
            if(bit_cnt==8) { buffer.push_back((uint8_t)bit_acc); bit_acc=0; bit_cnt=0; }
        }
    }
    inline void Flush() { if(bit_cnt>0) buffer.push_back((uint8_t)bit_acc); }
    const std::vector<uint8_t>& GetData() const { return buffer; }
};

// --- NEURAL PREDICTOR ---
class NeuralPredictor {
    static const int ORDER = 12;
    int32_t weights[ORDER];
    int32_t history[ORDER];
public:
    NeuralPredictor() { memset(weights,0,sizeof(weights)); memset(history,0,sizeof(history)); }
    inline int32_t Predict() {
        int64_t sum = 0;
        for(int i=0; i<ORDER; i++) sum += (int64_t)history[i] * weights[i];
        return (int32_t)(sum >> 11);
    }
    inline void Update(int32_t actual, int32_t pred) {
        int32_t err = actual - pred;
        int sign = (err > 0) ? 1 : ((err < 0) ? -1 : 0);
        if(sign == 0) return;
        int delta = (std::abs(err) > 256) ? 8 : 2;
        for(int i=0; i<ORDER; i++) {
            int h_sign = (history[i]>0)?1:((history[i]<0)?-1:0);
            if(sign == h_sign) weights[i] += delta;
            else if(h_sign != 0) weights[i] -= delta;
            if((i & 7) == 0) { if(weights[i]>0) weights[i]--; if(weights[i]<0) weights[i]++; }
        }
        for(int i=ORDER-1; i>0; i--) history[i] = history[i-1];
        history[0] = actual;
    }
};

class VeloxCodec {
    static inline uint32_t ZigZag(int64_t n) { return (uint32_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint32_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    // --- ENCODER (Không thay đổi) ---
    static void EncodeStream(std::vector<velox_sample_t>& data, BitStreamWriter& bs) {
        int shift = LSBShifter::Analyze(data);
        LSBShifter::Apply(data, shift);
        bs.Write(shift, 5);

        NeuralPredictor ai;
        uint32_t run_avg = 512;

        for(auto val : data) {
            int32_t pred = ai.Predict();
            int32_t resid = (int32_t)val - pred;
            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }
            
            uint32_t m = ZigZag(resid);
            uint32_t q = m >> k;
            uint32_t r = m & ((1<<k)-1);

            if(q < 32) {
                for(uint32_t i=0;i<q;i++) bs.Write(1,1);
                bs.Write(0,1);
                if(k>0) bs.Write(r, k);
            } else {
                for(uint32_t i=0;i<32;i++) bs.Write(1,1);
                bs.Write(0,1);
                bs.Write(m, 32); 
            }
            ai.Update((int32_t)val, pred);
            run_avg = run_avg - (run_avg>>3) + (m>>3);
            if(run_avg < 1) run_avg = 1;
        }
    }

    static void EncodeRLE(const std::vector<uint8_t>& data, BitStreamWriter& bs) {
        if(data.empty()) return;
        uint8_t last = data[0]; int run = 0;
        for(size_t i=0; i<data.size(); i++) {
            if(data[i] == last && run < 255) run++;
            else { bs.Write(run, 8); bs.Write(last, 8); last = data[i]; run = 1; }
        }
        bs.Write(run, 8); bs.Write(last, 8);
    }

public:
    static std::vector<uint8_t> EncodeBlock(const std::vector<velox_sample_t>& samples, bool is_float, const std::vector<uint8_t>& exps) {
        BitStreamWriter bs;
        bs.Write(is_float, 1);
        if(is_float) EncodeRLE(exps, bs);
        std::vector<velox_sample_t> work = samples;
        EncodeStream(work, bs);
        bs.Flush();
        return bs.GetData();
    }

    // --- STREAMING DECODER (Hỗ trợ Seek) ---
    class StreamingDecoder {
        BitStreamReader bs;
        NeuralPredictor ai;
        uint32_t run_avg = 512;
        int lsb_shift = 0;
        bool is_float = false;
        std::vector<uint8_t> exponents;
        size_t exp_idx = 0;
        size_t total_samples = 0;
        size_t decoded_count = 0;
        
        // Lưu vị trí bắt đầu data sau header để seek
        // Vì BitStreamReader của chúng ta đơn giản, Reset() sẽ về 0.
        // Ta cần logic skip header.
        // Để đơn giản: Ta decode từ đầu mỗi khi seek.

    public:
        StreamingDecoder(const uint8_t* data, size_t size, size_t total) 
            : bs(data, size), total_samples(total) {
            Init();
        }

        void Init() {
            bs.Reset(); 
            is_float = bs.Read(1);
            exponents.clear();
            exp_idx = 0;
            decoded_count = 0;
            run_avg = 512;
            ai = NeuralPredictor(); 

            if (is_float) {
                while(exponents.size() < total_samples) {
                    int run = bs.Read(8); int val = bs.Read(8);
                    for(int i=0; i<run && exponents.size() < total_samples; i++) 
                        exponents.push_back(val);
                }
            }
            lsb_shift = bs.Read(5);
        }

        bool IsFloat() const { return is_float; }
        size_t GetDecodedCount() const { return decoded_count; }

        void SeekToSample(size_t target_sample) {
            if (target_sample < decoded_count) {
                Init();
            }
            
            while (decoded_count < target_sample && decoded_count < total_samples) {
                int k = 0;
                if (run_avg > 0) { k = 31 - __builtin_clz(run_avg); if (k < 0) k = 0; }

                uint32_t q = 0;
                while (bs.ReadBit()) q++;
                uint32_t m = (q < 32) ? ((q << k) | ((k>0)?bs.Read(k):0)) : bs.Read(32);

                int32_t resid = (int32_t)DeZigZag(m);
                int32_t pred = ai.Predict();
                int32_t val = resid + pred;

                ai.Update(val, pred);
                run_avg = run_avg - (run_avg >> 3) + (m >> 3);
                if (run_avg < 1) run_avg = 1;
                
                decoded_count++;
                if(is_float) exp_idx++;
            }
        }

        bool DecodeNext(velox_sample_t& out_val, uint8_t& out_exp) {
            if (decoded_count >= total_samples) return false;

            int k = 0;
            if (run_avg > 0) { k = 31 - __builtin_clz(run_avg); if (k < 0) k = 0; }

            uint32_t q = 0;
            while (bs.ReadBit()) q++;
            uint32_t m = (q < 32) ? ((q << k) | ((k>0)?bs.Read(k):0)) : bs.Read(32);

            int32_t resid = (int32_t)DeZigZag(m);
            int32_t pred = ai.Predict();
            int32_t val = resid + pred;

            ai.Update(val, pred);
            run_avg = run_avg - (run_avg >> 3) + (m >> 3);
            if (run_avg < 1) run_avg = 1;

            out_val = ((velox_sample_t)val) << lsb_shift;
            
            if (is_float && exp_idx < exponents.size()) out_exp = exponents[exp_idx++];
            else out_exp = 0;

            decoded_count++;
            return true;
        }
    };
    
    static void DecodeBlock(const uint8_t* data, size_t size, size_t count, 
        std::vector<velox_sample_t>& s, std::vector<uint8_t>& e, bool& f) {
        StreamingDecoder d(data, size, count);
        f = d.IsFloat(); s.resize(count); e.resize(count);
        for(size_t i=0; i<count; i++) d.DecodeNext(s[i], e[i]);
    }
};

#endif