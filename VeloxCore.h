#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"
#include "VeloxAdvanced.h"
#include "VeloxEntropy.h"  

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

// --- CORE CODEC ---
class VeloxCodec {
    // LPC Calculation
    static void ComputeLPC(const std::vector<velox_sample_t>& data, int order, std::vector<int>& coeffs, int& shift) {
        if(data.empty()) return;
        double autocorr[13];
        int stride = (data.size() > 4096) ? 2 : 1;
        for (int i = 0; i <= order; ++i) {
            double sum = 0;
            for (size_t j = i; j < data.size(); j+=stride) sum += (double)data[j] * data[j - i];
            autocorr[i] = sum;
        }
        if(std::abs(autocorr[0]) < 1e-9) { shift=0; coeffs.assign(order, 0); return; }
        double a[13][13] = {0}; double e[13] = {0}; e[0] = autocorr[0];
        for (int i = 1; i <= order; ++i) {
            double k = autocorr[i];
            for (int j = 1; j < i; ++j) k -= a[j][i - 1] * autocorr[i - j];
            k /= e[i - 1];
            if(k > 0.999) k = 0.999; if(k < -0.999) k = -0.999;
            a[i][i] = k;
            for (int j = 1; j < i; ++j) a[j][i] = a[j][i - 1] - k * a[i - j][i - 1];
            e[i] = e[i - 1] * (1 - k * k);
        }
        shift = 11; coeffs.resize(order);
        for (int i = 1; i <= order; ++i) coeffs[i-1] = (int)std::floor(a[i][order] * (1 << shift) + 0.5);
    }

    // --- ENCODE CHANNEL ---
    static void EncodeChannel(std::vector<velox_sample_t>& data, 
                              std::vector<velox_sample_t>& history,
                              BitStreamWriter& bs) {
        
        // 1. Sparse Check
        if (VeloxOptimizer::IsSilence(data)) {
            bs.Write(1, 1); 
            history.insert(history.end(), data.begin(), data.end());
            if (history.size() > 100000) history.erase(history.begin(), history.begin() + data.size());
            return;
        }
        bs.Write(0, 1); 

        // 2. LTP Search 
        auto match = VeloxOptimizer::FindBestMatch(history, data);
        if (match.found) {
            bs.Write(1, 1); 
            bs.Write(match.lag, 16); 
            VeloxOptimizer::ApplyLTP(data, history, match.lag);
        } else {
            bs.Write(0, 1); // Flag 0: No LTP
        }

        int shift_lsb = LSBShifter::Analyze(data);
        LSBShifter::Apply(data, shift_lsb);
        bs.Write(shift_lsb, 5);

        int order = 8;
        int lpc_shift = 0;
        std::vector<int> lpc_coeffs;
        ComputeLPC(data, order, lpc_coeffs, lpc_shift);
        bs.Write(lpc_shift, 5);
        for(int c : lpc_coeffs) bs.Write(c & 0xFFFF, 16);

        NeuralPredictor neural;
        uint32_t run_avg = 512;

        for(size_t i=0; i<data.size(); i++) {
            velox_sample_t original = data[i];
            
            // LPC Predict
            int64_t sum = 0;
            for(int j=0; j<order; j++) {
                if(i > (size_t)j) sum += (int64_t)lpc_coeffs[j] * data[i-1-j];
            }
            int32_t predLPC = (int32_t)(sum >> lpc_shift);
            int32_t resLPC = (int32_t)original - predLPC;

            int32_t predNeural = neural.Predict();
            int32_t finalRes = resLPC - predNeural;

            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }
            
            VeloxEntropy::EncodeSample(bs, finalRes, k);

            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg>>3) + (VeloxEntropy::ZigZag(finalRes)>>3);
            if(run_avg < 1) run_avg = 1;
        }
    }

    // --- DECODE CHANNEL ---
    static void DecodeChannel(BitStreamReader& bs, size_t count, 
                              std::vector<velox_sample_t>& out,
                              std::vector<velox_sample_t>& history) {
        out.resize(count);

        int is_silence = bs.ReadBit();
        if (is_silence) {
            std::fill(out.begin(), out.end(), 0);
            history.insert(history.end(), out.begin(), out.end());
            if (history.size() > 100000) history.erase(history.begin(), history.begin() + count);
            return;
        }

        int use_ltp = bs.ReadBit();
        int ltp_lag = 0;
        if (use_ltp) ltp_lag = bs.Read(16);

        int shift_lsb = bs.Read(5);

        int order = 8;
        int lpc_shift = bs.Read(5);
        std::vector<int> lpc_coeffs(order);
        for(int i=0; i<order; i++) lpc_coeffs[i] = bs.ReadS(16);

        NeuralPredictor neural;
        uint32_t run_avg = 512;

        for(size_t i=0; i<count; i++) {
            int k = 0;
            if(run_avg > 0) { k = 31 - __builtin_clz(run_avg); if(k<0) k=0; }

            int32_t finalRes = VeloxEntropy::DecodeSample(bs, k);
            
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
            run_avg = run_avg - (run_avg>>3) + (VeloxEntropy::ZigZag(finalRes)>>3);
            if(run_avg < 1) run_avg = 1;
        }

        LSBShifter::Restore(out, shift_lsb);

        if (use_ltp) {
            VeloxOptimizer::RestoreLTP(out, history, ltp_lag);
        }

        // Update History
        history.insert(history.end(), out.begin(), out.end());
        if (history.size() > 100000) history.erase(history.begin(), history.begin() + count);
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
    static std::vector<uint8_t> DecodeRLE(BitStreamReader& bs, size_t count) {
        std::vector<uint8_t> out; out.reserve(count);
        while(out.size() < count) {
            int run = bs.Read(8); int val = bs.Read(8);
            for(int i=0; i<run; i++) out.push_back(val);
        }
        return out;
    }

public:
    // --- STATEFUL STREAMING CODEC ---
    
    class Encoder {
        std::vector<velox_sample_t> histL, histR;
    public:
        std::vector<uint8_t> ProcessBlock(const std::vector<velox_sample_t>& samples, bool is_float, const std::vector<uint8_t>& exps) {
            BitStreamWriter bs;
            bs.Write(is_float, 1);
            if(is_float) EncodeRLE(exps, bs);

            size_t total = samples.size();
            const size_t SUB_BLOCK = 4096;
            
            if(total % 2 != 0) {
                std::vector<velox_sample_t> mono = samples;
                std::vector<velox_sample_t> histL_copy = histL; 
                EncodeChannel(mono, histL, bs);
                bs.Flush(); return bs.GetData();
            }

            for(size_t i=0; i<total; i += SUB_BLOCK) {
                size_t end = std::min(i + SUB_BLOCK, total);
                if((end - i) % 2 != 0) end--;
                size_t len = (end - i) / 2;
                
                std::vector<velox_sample_t> chL, chR;
                chL.reserve(len); chR.reserve(len);
                uint64_t sad_LR = 0, sad_MS = 0;

                for(size_t j=0; j<len; j++) {
                    velox_sample_t L = samples[i + j*2];
                    velox_sample_t R = samples[i + j*2 + 1];
                    chL.push_back(L); chR.push_back(R);
                    sad_LR += std::abs(L) + std::abs(R);
                    sad_MS += std::abs((L+R)>>1) + std::abs(L-R);
                }

                bool use_MS = (sad_MS < sad_LR);
                bs.Write(use_MS, 1);

                if(use_MS) {
                    for(size_t j=0; j<len; j++) {
                        velox_sample_t L = chL[j]; velox_sample_t R = chR[j];
                        chL[j] = (L+R)>>1; chR[j] = L-R;
                    }
                }
                
                EncodeChannel(chL, histL, bs);
                EncodeChannel(chR, histR, bs);
            }
            bs.Flush(); return bs.GetData();
        }
    };

    class StreamingDecoder {
        BitStreamReader bs;
        std::vector<velox_sample_t> histL, histR;
        std::vector<uint8_t> exponents;
        size_t total_samples;
        size_t decoded_count = 0;
        size_t exp_idx = 0;
        bool is_float;
        
        std::vector<velox_sample_t> blockBuffer;
        size_t blockPtr = 0;

    public:
        StreamingDecoder(const uint8_t* data, size_t size, size_t total) 
            : bs(data, size), total_samples(total) {
            is_float = bs.Read(1);
            if (is_float) exponents = DecodeRLE(bs, total); 
        }

        bool IsFloat() const { return is_float; }

        bool DecodeNext(velox_sample_t& out_val, uint8_t& out_exp) {
            if (decoded_count >= total_samples) return false;

            if (blockPtr >= blockBuffer.size()) {
                blockBuffer.clear();
                
                size_t remaining = total_samples - decoded_count;
                if (remaining == 0) return false;
                
                int use_MS = bs.Read(1);
                
                size_t CHUNK = 4096; 
                size_t current_chunk = std::min(remaining, CHUNK);
                if (current_chunk % 2 != 0) current_chunk--; // Chẵn
                size_t frames = current_chunk / 2;

                std::vector<velox_sample_t> c1, c2;
                DecodeChannel(bs, frames, c1, histL);
                DecodeChannel(bs, frames, c2, histR);

                for(size_t j=0; j<frames; j++) {
                    velox_sample_t v1 = c1[j];
                    velox_sample_t v2 = c2[j];
                    if(use_MS) {
                        blockBuffer.push_back(v1 + ((v2+1)>>1)); // L
                        blockBuffer.push_back(v1 - (v2>>1));     // R
                    } else {
                        blockBuffer.push_back(v1);
                        blockBuffer.push_back(v2);
                    }
                }
                blockPtr = 0;
            }

            out_val = blockBuffer[blockPtr++];
            
            if (is_float && exp_idx < exponents.size()) out_exp = exponents[exp_idx++];
            else out_exp = 0;

            decoded_count++;
            return true;
        }
    };
};

#endif