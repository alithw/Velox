#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxFormat.h"
#include "VeloxAdvanced.h"
#include "VeloxEntropy.h"
#include "VeloxThreads.h"
#include <numeric>
#include <future>

// --- NEURAL PREDICTOR (Giữ nguyên) ---
class NeuralPredictor
{
    static const int ORDER = 12;
    int32_t weights[ORDER];
    int32_t history[ORDER];

public:
    NeuralPredictor()
    {
        memset(weights, 0, sizeof(weights));
        memset(history, 0, sizeof(history));
    }
    inline int32_t Predict()
    {
        int64_t sum = 0;
        for (int i = 0; i < ORDER; i++)
            sum += (int64_t)history[i] * weights[i];
        return (int32_t)(sum >> 11);
    }
    inline void Update(int32_t actual, int32_t pred)
    {
        int32_t err = actual - pred;
        int sign = (err > 0) ? 1 : ((err < 0) ? -1 : 0);
        if (sign == 0)
            return;
        int delta = (std::abs(err) > 1024) ? 16 : 4;
        for (int i = 0; i < ORDER; i++)
        {
            int h_sign = (history[i] > 0) ? 1 : ((history[i] < 0) ? -1 : 0);
            if (sign == h_sign)
                weights[i] += delta;
            else if (h_sign != 0)
                weights[i] -= delta;
            if ((i & 7) == 0)
            {
                if (weights[i] > 0)
                    weights[i]--;
                if (weights[i] < 0)
                    weights[i]++;
            }
        }
        for (int i = ORDER - 1; i > 0; i--)
            history[i] = history[i - 1];
        history[0] = actual;
    }
};

class VeloxCodec
{
    static inline uint32_t ZigZag(int64_t n) { return (uint32_t)((n << 1) ^ (n >> 63)); }
    static inline int64_t DeZigZag(uint32_t n) { return (int64_t)((n >> 1) ^ -(int64_t)(n & 1)); }

    static void ComputeLPC(const std::vector<velox_sample_t> &data, int order, std::vector<int> &coeffs, int &shift)
    {
        if (data.empty())
            return;
        double autocorr[13];
        int stride = (data.size() > 4096) ? 4 : 1;
        for (int i = 0; i <= order; ++i)
        {
            double sum = 0;
            for (size_t j = i; j < data.size(); j += stride)
                sum += (double)data[j] * data[j - i];
            autocorr[i] = sum;
        }
        if (std::abs(autocorr[0]) < 1e-9)
        {
            shift = 0;
            coeffs.assign(order, 0);
            return;
        }
        double a[13][13] = {0};
        double e[13] = {0};
        e[0] = autocorr[0];
        for (int i = 1; i <= order; ++i)
        {
            double k = autocorr[i];
            for (int j = 1; j < i; ++j)
                k -= a[j][i - 1] * autocorr[i - j];
            k /= e[i - 1];
            if (k > 0.999)
                k = 0.999;
            if (k < -0.999)
                k = -0.999;
            a[i][i] = k;
            for (int j = 1; j < i; ++j)
                a[j][i] = a[j][i - 1] - k * a[i - j][i - 1];
            e[i] = e[i - 1] * (1 - k * k);
        }
        shift = 11;
        coeffs.resize(order);
        for (int i = 1; i <= order; ++i)
            coeffs[i - 1] = (int)std::floor(a[i][order] * (1 << shift) + 0.5);
    }

    static void TryCompressChannel(const std::vector<velox_sample_t> &input_data, BitStreamWriter &bs, bool high_res_mode)
    {
        std::vector<velox_sample_t> work_data = input_data;
        std::vector<uint8_t> low_bits;

        if (high_res_mode)
        {
            low_bits.reserve(work_data.size());
            for (auto &val : work_data)
            {
                low_bits.push_back((uint8_t)(val & 0xFF));
                val >>= 8;
            }
        }

        if (VeloxOptimizer::IsSilence(work_data))
        {
            bs.Write(1, 1);
            return;
        }
        bs.Write(0, 1);

        int shift_lsb = LSBShifter::Analyze(work_data);
        LSBShifter::Apply(work_data, shift_lsb);
        bs.Write(shift_lsb, 5);

        int order = 8;
        int lpc_shift = 0;
        std::vector<int> lpc_coeffs;
        ComputeLPC(work_data, order, lpc_coeffs, lpc_shift);

        // Simple Order: 8
        bs.Write(lpc_shift, 5);
        for (int c : lpc_coeffs)
            bs.Write(c & 0xFFFF, 16);

        NeuralPredictor neural;
        uint32_t run_avg = 512;

        for (size_t i = 0; i < work_data.size(); i++)
        {
            velox_sample_t original = work_data[i];
            int64_t sum = 0;
            for (int j = 0; j < order; j++)
            {
                if (i > (size_t)j)
                    sum += (int64_t)lpc_coeffs[j] * work_data[i - 1 - j];
            }
            int32_t predLPC = (int32_t)(sum >> lpc_shift);
            int32_t resLPC = (int32_t)original - predLPC;
            int32_t predNeural = neural.Predict();
            int32_t finalRes = resLPC - predNeural;

            int k = 0;
            if (run_avg > 0)
            {
                k = 31 - __builtin_clz(run_avg);
                if (k < 0)
                    k = 0;
            }
            VeloxEntropy::EncodeSample(bs, finalRes, k);

            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg >> 3) + (VeloxEntropy::ZigZag(finalRes) >> 3);
            if (run_avg < 1)
                run_avg = 1;
        }

        if (high_res_mode)
        {
            for (uint8_t b : low_bits)
                bs.Write(b, 8);
        }
    }

    static void DecodeChannelWorker(BitStreamReader &bs, size_t count, std::vector<velox_sample_t> &out, bool high_res_mode)
    {
        out.resize(count);
        int is_silence = bs.ReadBit();
        if (is_silence)
        {
            std::fill(out.begin(), out.end(), 0);
            return;
        }

        int shift_lsb = bs.Read(5);
        int order = 8;
        int lpc_shift = bs.Read(5);
        std::vector<int> lpc_coeffs(order);
        for (int i = 0; i < order; i++)
            lpc_coeffs[i] = bs.ReadS(16);

        NeuralPredictor neural;
        uint32_t run_avg = 512;

        for (size_t i = 0; i < count; i++)
        {
            int k = 0;
            if (run_avg > 0)
            {
                k = 31 - __builtin_clz(run_avg);
                if (k < 0)
                    k = 0;
            }
            int32_t finalRes = VeloxEntropy::DecodeSample(bs, k);

            int32_t predNeural = neural.Predict();
            int32_t resLPC = finalRes + predNeural;
            int64_t sum = 0;
            for (int j = 0; j < order; j++)
            {
                if (i > (size_t)j)
                    sum += (int64_t)lpc_coeffs[j] * out[i - 1 - j];
            }
            int32_t val = resLPC + (int32_t)(sum >> lpc_shift);
            out[i] = val;
            neural.Update(resLPC, predNeural);
            run_avg = run_avg - (run_avg >> 3) + (VeloxEntropy::ZigZag(finalRes) >> 3);
            if (run_avg < 1)
                run_avg = 1;
        }

        LSBShifter::Restore(out, shift_lsb);

        if (high_res_mode)
        {
            for (size_t i = 0; i < count; i++)
            {
                uint8_t low = bs.Read(8);
                out[i] = (out[i] << 8) | low;
            }
        }
    }

    // --- FIX RAW BLOCK WRITER ---
    static void WriteRawBlock(const std::vector<velox_sample_t> &samples, BitStreamWriter &bs)
    {
        // ĐÃ XÓA: bs.Write(0, 1); (Vì Lambda đã ghi cờ này rồi)
        for (auto s : samples)
            bs.Write((uint32_t)((s << 1) ^ (s >> 63)), 32);
    }

    // ReadRawBlock (Giữ nguyên)
    static void ReadRawBlock(BitStreamReader &bs, size_t count, std::vector<velox_sample_t> &out)
    {
        out.resize(count);
        for (size_t i = 0; i < count; i++)
        {
            uint32_t z = bs.Read(32);
            int32_t s32 = (int32_t)z;
            out[i] = (int64_t)((s32 >> 1) ^ -(s32 & 1));
        }
    }

    // Helpers RLE (Giữ nguyên)
    static void EncodeRLE(const std::vector<uint8_t> &data, BitStreamWriter &bs)
    {
        if (data.empty())
            return;
        uint8_t last = data[0];
        int run = 0;
        for (size_t i = 0; i < data.size(); i++)
        {
            if (data[i] == last && run < 255)
                run++;
            else
            {
                bs.Write(run, 8);
                bs.Write(last, 8);
                last = data[i];
                run = 1;
            }
        }
        bs.Write(run, 8);
        bs.Write(last, 8);
    }
    static std::vector<uint8_t> DecodeRLE(BitStreamReader &bs, size_t count)
    {
        std::vector<uint8_t> out;
        out.reserve(count);
        while (out.size() < count)
        {
            int run = bs.Read(8);
            int val = bs.Read(8);
            for (int i = 0; i < run; i++)
                out.push_back(val);
        }
        return out;
    }

public:
    class Encoder
    {
    public:
        static ThreadPool &GetPool()
        {
            static ThreadPool pool(std::thread::hardware_concurrency());
            return pool;
        }

        std::vector<uint8_t> ProcessBlock(std::vector<velox_sample_t> &samples, bool is_float,
                                          const std::vector<uint8_t> &exps, const uint8_t *raw_bytes)
        {
            BitStreamWriter bs;

            // SMART FLOAT
            int float_mode = 0;
            if (is_float)
            {
                int detected = FormatHandler::DetectPseudoFloat(raw_bytes, samples.size());
                if (detected == 16)
                {
                    float_mode = 1;
                    FormatHandler::DemoteFloatToInt(raw_bytes, samples.size(), 16, samples);
                }
                else if (detected == 24)
                {
                    float_mode = 2;
                    FormatHandler::DemoteFloatToInt(raw_bytes, samples.size(), 24, samples);
                }
            }

            bs.Write(is_float, 1);
            if (is_float)
            {
                bs.Write(float_mode, 2);
                if (float_mode == 0)
                    EncodeRLE(exps, bs);
            }

            bool high_res_mode = false;
            if (!is_float || float_mode > 0)
            {
                for (auto s : samples)
                    if (std::abs(s) > 65536)
                    {
                        high_res_mode = true;
                        break;
                    }
            }
            bs.Write(high_res_mode, 1);

            size_t total = samples.size();
            const size_t SUB_BLOCK = 8192;
            std::vector<std::future<std::vector<uint8_t>>> futures;

            if (total % 2 != 0)
            {
                auto task = [samples, high_res_mode]()
                {
                    BitStreamWriter bTemp;
                    bTemp.Write(1, 1);
                    TryCompressChannel(samples, bTemp, high_res_mode);
                    bTemp.Flush();
                    if (bTemp.GetData().size() > samples.size() * 4)
                    {
                        BitStreamWriter bRaw;
                        bRaw.Write(0, 1); // Raw Flag
                        WriteRawBlock(samples, bRaw);
                        bRaw.Flush();
                        return bRaw.GetData();
                    }
                    return bTemp.GetData();
                };
                futures.push_back(GetPool().enqueue(task));
            }
            else
            {
                for (size_t i = 0; i < total; i += SUB_BLOCK)
                {
                    size_t end = std::min(i + SUB_BLOCK, total);
                    if ((end - i) % 2 != 0)
                        end--;
                    size_t len = (end - i) / 2;

                    std::vector<velox_sample_t> chunkL, chunkR;
                    chunkL.reserve(len);
                    chunkR.reserve(len);
                    uint64_t sad_LR = 0, sad_MS = 0;

                    for (size_t j = 0; j < len; j++)
                    {
                        velox_sample_t L = samples[i + j * 2];
                        velox_sample_t R = samples[i + j * 2 + 1];
                        chunkL.push_back(L);
                        chunkR.push_back(R);
                        sad_LR += std::abs(L) + std::abs(R);
                        sad_MS += std::abs((L + R) >> 1) + std::abs(L - R);
                    }

                    bool use_MS = (sad_MS < sad_LR);
                    if (use_MS)
                    {
                        for (size_t j = 0; j < len; j++)
                        {
                            velox_sample_t L = chunkL[j];
                            velox_sample_t R = chunkR[j];
                            chunkL[j] = (L + R) >> 1;
                            chunkR[j] = L - R;
                        }
                    }

                    auto task = [chunkL, chunkR, use_MS, high_res_mode]()
                    {
                        BitStreamWriter bTemp;
                        bTemp.Write(1, 1);
                        bTemp.Write(use_MS, 1);
                        TryCompressChannel(chunkL, bTemp, high_res_mode);
                        TryCompressChannel(chunkR, bTemp, high_res_mode);
                        bTemp.Flush();

                        size_t rawSize = (chunkL.size() + chunkR.size()) * 4;
                        if (bTemp.GetData().size() >= rawSize)
                        {
                            BitStreamWriter bRaw;
                            bRaw.Write(0, 1);
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

            for (auto &f : futures)
            {
                auto data = f.get();
                bs.Write((uint32_t)data.size(), 32);
                for (uint8_t b : data)
                    bs.Write(b, 8);
            }

            bs.Flush();
            return bs.GetData();
        }
    };

    class StreamingDecoder
    {
        BitStreamReader bs;
        std::vector<uint8_t> exponents;
        size_t total_samples;
        size_t decoded_count = 0;
        size_t exp_idx = 0;
        bool is_float;
        int float_mode;
        bool high_res_mode;
        std::vector<velox_sample_t> blockBuffer;
        size_t blockPtr = 0;

    public:
        StreamingDecoder(const uint8_t *data, size_t size, size_t total)
            : bs(data, size), total_samples(total)
        {
            is_float = bs.Read(1);
            if (is_float)
            {
                float_mode = bs.Read(2);
                if (float_mode == 0)
                    exponents = DecodeRLE(bs, total);
            }
            high_res_mode = bs.Read(1);
        }

        bool IsFloat() const { return is_float && (float_mode == 0); }
        int GetFloatMode() const { return float_mode; }

        bool DecodeNext(velox_sample_t &out_val, uint8_t &out_exp)
        {
            if (decoded_count >= total_samples)
                return false;

            if (blockPtr >= blockBuffer.size())
            {
                blockBuffer.clear();
                uint32_t chunkSize = bs.Read(32);
                if (chunkSize == 0)
                    return false;

                std::vector<uint8_t> chunkData;
                chunkData.reserve(chunkSize);
                for (uint32_t i = 0; i < chunkSize; i++)
                    chunkData.push_back((uint8_t)bs.Read(8));
                BitStreamReader bChunk(chunkData.data(), chunkSize);

                int mode = bChunk.ReadBit();
                size_t remaining = total_samples - decoded_count;
                size_t frames = std::min((size_t)4096, remaining / 2);
                if (frames == 0 && remaining > 0)
                    frames = remaining;

                if (mode == 1)
                { // Compressed
                    int use_MS = bChunk.ReadBit();
                    std::vector<velox_sample_t> c1, c2;
                    DecodeChannelWorker(bChunk, frames, c1, high_res_mode);
                    DecodeChannelWorker(bChunk, frames, c2, high_res_mode);
                    for (size_t j = 0; j < frames; j++)
                    {
                        if (use_MS)
                        {
                            blockBuffer.push_back(c1[j] + ((c2[j] + 1) >> 1));
                            blockBuffer.push_back(c1[j] - (c2[j] >> 1));
                        }
                        else
                        {
                            blockBuffer.push_back(c1[j]);
                            blockBuffer.push_back(c2[j]);
                        }
                    }
                }
                else
                { // Raw
                    int use_MS = bChunk.ReadBit();
                    std::vector<velox_sample_t> c1, c2;
                    ReadRawBlock(bChunk, frames, c1);
                    ReadRawBlock(bChunk, frames, c2);
                    for (size_t j = 0; j < frames; j++)
                    {
                        if (use_MS)
                        {
                            blockBuffer.push_back(c1[j] + ((c2[j] + 1) >> 1));
                            blockBuffer.push_back(c1[j] - (c2[j] >> 1));
                        }
                        else
                        {
                            blockBuffer.push_back(c1[j]);
                            blockBuffer.push_back(c2[j]);
                        }
                    }
                }
                blockPtr = 0;
            }

            out_val = blockBuffer[blockPtr++];
            if (is_float && float_mode == 0 && exp_idx < exponents.size())
                out_exp = exponents[exp_idx++];
            else
                out_exp = 0;

            decoded_count++;
            return true;
        }
    };
};

#endif