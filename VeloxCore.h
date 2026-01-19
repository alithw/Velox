#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"
#include "VeloxAdvanced.h"
#include "VeloxEntropy.h"
#include "VeloxThreads.h"
#include <numeric>
#include <future>

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
        // Tăng tốc độ học cho PCM 24/32
        int delta = (std::abs(err) > 1024) ? 16 : 4; 
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

    static void ComputeLPC(const std::vector<velox_sample_t>& data, int order, std::vector<int>& coeffs, int& shift) {
        if(data.empty()) return;
        double autocorr[13];
        int stride = (data.size() > 4096) ? 4 : 1; 
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

    // --- WORKER: Cố gắng nén ---
    static void TryCompressChannel(const std::vector<velox_sample_t>& input_data, BitStreamWriter& bs, bool high_res_mode) {
        std::vector<velox_sample_t> work_data = input_data;
        std::vector<uint8_t> low_bits;
        
        // 1. Bit-Plane Slicing (Chỉ khi được bật)
        if (high_res_mode) {
            low_bits.reserve(work_data.size());
            for(auto& val : work_data) {
                low_bits.push_back((uint8_t)(val & 0xFF));
                val >>= 8;
            }
        }

        // 2. Sparse Check
        if (VeloxOptimizer::IsSilence(work_data)) {
            bs.Write(1, 1); // Flag: Silence
            return; 
        }
        bs.Write(0, 1); // Flag: Data

        // 3. LSB Shift
        int shift_lsb = LSBShifter::Analyze(work_data);
        LSBShifter::Apply(work_data, shift_lsb);
        bs.Write(shift_lsb, 5);

        // 4. LPC + Neural
        int order = 8; int lpc_shift = 0;
        std::vector<int> lpc_coeffs;
        ComputeLPC(work_data, order, lpc_coeffs, lpc_shift);
        bs.Write(lpc_shift, 5);
        for(int c : lpc_coeffs) bs.Write(c & 0xFFFF, 16);

        NeuralPredictor neural;
        uint32_t run_avg = 512;

        for(size_t i=0; i<work_data.size(); i++) {
            velox_sample_t original = work_data[i];
            int64_t sum = 0;
            for(int j=0; j<order; j++) {
                if(i > (size_t)j) sum += (int64_t)lpc_coeffs[j] * work_data[i-1-j];
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

        // 5. Low Bits
        if (high_res_mode) {
            for(uint8_t b : low_bits) bs.Write(b, 8);
        }
    }

    // --- WORKER: Giải nén ---
    static void DecodeChannelWorker(BitStreamReader& bs, size_t count, std::vector<velox_sample_t>& out, bool high_res_mode) {
        out.resize(count);
        int is_silence = bs.ReadBit();
        if(is_silence) { std::fill(out.begin(), out.end(), 0); return; }

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
            int32_t val = resLPC + (int32_t)(sum >> lpc_shift);
            out[i] = val;
            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg>>3) + (VeloxEntropy::ZigZag(finalRes)>>3);
            if(run_avg < 1) run_avg = 1;
        }

        LSBShifter::Restore(out, shift_lsb);

        if (high_res_mode) {
            for(size_t i=0; i<count; i++) {
                uint8_t low = bs.Read(8);
                out[i] = (out[i] << 8) | low;
            }
        }
    }

    // Helper RLE
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

    // Ghi Raw (Verbatim) nếu nén thất bại
    static void WriteRawBlock(const std::vector<velox_sample_t>& samples, BitStreamWriter& bs) {
        // Mode 0: Raw
        bs.Write(0, 1); 
        // Vì đây là block raw 64-bit int nội bộ, ta ghi zigzag để tiết kiệm nếu số nhỏ
        // Nhưng để chắc ăn, ghi raw byte luôn? Không, ghi Zigzag an toàn hơn.
        for(auto s : samples) {
            uint64_t z = (uint64_t)((s << 1) ^ (s >> 63));
            // Ghi 64 bit là quá nhiều.
            // Ta chỉ cần ghi tối đa số bit thực tế.
            // Giả sử max 32 bit cho audio thường.
            // Để đơn giản và nhanh: Ghi fixed 32 bit (đủ cho float/int24/int32)
            bs.Write((uint32_t)z, 32); 
        }
    }

    static void ReadRawBlock(BitStreamReader& bs, size_t count, std::vector<velox_sample_t>& out) {
        out.resize(count);
        for(size_t i=0; i<count; i++) {
            uint32_t z = bs.Read(32);
            // DeZigZag 32 to 64
            int32_t s32 = (int32_t)z; // Cast back
            out[i] = (int64_t)((s32 >> 1) ^ -(s32 & 1));
        }
    }

public:
    class Encoder {
    public:
        static ThreadPool& GetPool() {
            static ThreadPool pool(std::thread::hardware_concurrency());
            return pool;
        }

        std::vector<uint8_t> ProcessBlock(const std::vector<velox_sample_t>& samples, bool is_float, const std::vector<uint8_t>& exps) {
            BitStreamWriter bs;
            bs.Write(is_float, 1);
            if(is_float) EncodeRLE(exps, bs);

            // AUTO DETECT HIGH-RES
            // Chỉ bật High-Res (Slicing) cho PCM 24/32-bit.
            // TẮT cho Float (Mantissa noise quá lớn, slicing làm tăng size)
            bool high_res_mode = false;
            if (!is_float) {
                for(auto s : samples) if(std::abs(s) > 65536) { high_res_mode = true; break; }
            }
            bs.Write(high_res_mode, 1);

            size_t total = samples.size();
            const size_t SUB_BLOCK = 8192; 
            std::vector<std::future<std::vector<uint8_t>>> futures;

            if(total % 2 != 0) { // Mono
                auto task = [samples, high_res_mode]() {
                    BitStreamWriter bTemp;
                    bTemp.Write(1, 1); // Flag 1: Compressed Mode
                    TryCompressChannel(samples, bTemp, high_res_mode);
                    bTemp.Flush();
                    
                    // Check expansion
                    if (bTemp.GetData().size() > samples.size() * 4) {
                        BitStreamWriter bRaw;
                        WriteRawBlock(samples, bRaw);
                        bRaw.Flush();
                        return bRaw.GetData();
                    }
                    return bTemp.GetData();
                };
                futures.push_back(GetPool().enqueue(task));
            } else { // Stereo
                for(size_t i=0; i<total; i += SUB_BLOCK) {
                    size_t end = std::min(i + SUB_BLOCK, total);
                    if((end - i) % 2 != 0) end--;
                    size_t len = (end - i) / 2;

                    std::vector<velox_sample_t> chunkL, chunkR;
                    chunkL.reserve(len); chunkR.reserve(len);
                    uint64_t sad_LR = 0, sad_MS = 0;

                    for(size_t j=0; j<len; j++) {
                        velox_sample_t L = samples[i + j*2]; velox_sample_t R = samples[i + j*2 + 1];
                        chunkL.push_back(L); chunkR.push_back(R);
                        sad_LR += std::abs(L) + std::abs(R);
                        sad_MS += std::abs((L+R)>>1) + std::abs(L-R);
                    }
                    
                    bool use_MS = (sad_MS < sad_LR);
                    if(use_MS) {
                        for(size_t j=0; j<len; j++) {
                            velox_sample_t L = chunkL[j]; velox_sample_t R = chunkR[j];
                            chunkL[j] = (L+R)>>1; chunkR[j] = L-R;
                        }
                    }

                    auto task = [chunkL, chunkR, use_MS, high_res_mode]() {
                        BitStreamWriter bTemp;
                        bTemp.Write(1, 1); // Mode Compressed
                        bTemp.Write(use_MS, 1);
                        TryCompressChannel(chunkL, bTemp, high_res_mode);
                        TryCompressChannel(chunkR, bTemp, high_res_mode);
                        bTemp.Flush();

                        // VERBATIM FALLBACK (Safety Valve)
                        // Nếu kích thước nén > kích thước gốc (Raw 32-bit * 2 channels * length)
                        size_t rawSize = (chunkL.size() + chunkR.size()) * 4;
                        if (bTemp.GetData().size() >= rawSize) {
                            BitStreamWriter bRaw;
                            bRaw.Write(0, 1); // Mode Raw (Verbatim)
                            // Restore original L/R if M/S was used? 
                            // No, just write whatever we have (M/S or L/R), decoder will reverse M/S later
                            // Wait, if we write Raw M/S, decoder needs to know use_MS?
                            // Simple: In Raw Mode, we ALWAYS write L/R interlaced for simplicity?
                            // Let's stick to block structure:
                            // Raw Mode Header: [0] [use_MS] [RawDataL] [RawDataR]
                            bRaw.Write(use_MS, 1);
                            WriteRawBlock(chunkL, bRaw);
                            WriteRawBlock(chunkR, bRaw);
                            bRaw.Flush();
                            return bRaw.GetData();
                        }
                        return bTemp.GetData();
                    };
                    futures.push_back(GetPool().enqueue(task));
                }
            }

            for(auto& f : futures) {
                auto data = f.get();
                bs.Write((uint32_t)data.size(), 32);
                for(uint8_t b : data) bs.Write(b, 8);
            }
            
            bs.Flush(); return bs.GetData();
        }
    };

    class StreamingDecoder {
        BitStreamReader bs;
        std::vector<uint8_t> exponents;
        size_t total_samples;
        size_t decoded_count = 0;
        size_t exp_idx = 0;
        bool is_float;
        bool high_res_mode;
        std::vector<velox_sample_t> blockBuffer;
        size_t blockPtr = 0;

    public:
        StreamingDecoder(const uint8_t* data, size_t size, size_t total) 
            : bs(data, size), total_samples(total) {
            is_float = bs.Read(1);
            if (is_float) exponents = DecodeRLE(bs, total);
            high_res_mode = bs.Read(1);
        }

        bool IsFloat() const { return is_float; }

        bool DecodeNext(velox_sample_t& out_val, uint8_t& out_exp) {
            if (decoded_count >= total_samples) return false;

            if (blockPtr >= blockBuffer.size()) {
                blockBuffer.clear();
                uint32_t chunkSize = bs.Read(32);
                if (chunkSize == 0) return false;

                // Create sub-stream
                // (Optimized: Just update pointer in main stream? No, current reader is simple)
                // Let's copy chunk
                std::vector<uint8_t> chunkData; chunkData.reserve(chunkSize);
                for(uint32_t i=0; i<chunkSize; i++) chunkData.push_back((uint8_t)bs.Read(8));
                BitStreamReader bChunk(chunkData.data(), chunkSize);

                int mode = bChunk.ReadBit(); // 1=Comp, 0=Raw
                
                size_t remaining = total_samples - decoded_count;
                // Stereo fallback logic
                size_t frames = std::min((size_t)4096, remaining / 2);
                if (frames == 0 && remaining > 0) frames = remaining; // Mono case

                if (mode == 1) { // Compressed
                    int use_MS = bChunk.ReadBit();
                    std::vector<velox_sample_t> c1, c2;
                    DecodeChannelWorker(bChunk, frames, c1, high_res_mode);
                    DecodeChannelWorker(bChunk, frames, c2, high_res_mode);
                    for(size_t j=0; j<frames; j++) {
                        if(use_MS) {
                            blockBuffer.push_back(c1[j] + ((c2[j]+1)>>1));
                            blockBuffer.push_back(c1[j] - (c2[j]>>1));
                        } else {
                            blockBuffer.push_back(c1[j]); blockBuffer.push_back(c2[j]);
                        }
                    }
                } else { // Raw
                    int use_MS = bChunk.ReadBit();
                    std::vector<velox_sample_t> c1, c2;
                    ReadRawBlock(bChunk, frames, c1);
                    ReadRawBlock(bChunk, frames, c2);
                    for(size_t j=0; j<frames; j++) {
                        if(use_MS) {
                            blockBuffer.push_back(c1[j] + ((c2[j]+1)>>1));
                            blockBuffer.push_back(c1[j] - (c2[j]>>1));
                        } else {
                            blockBuffer.push_back(c1[j]); blockBuffer.push_back(c2[j]);
                        }
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