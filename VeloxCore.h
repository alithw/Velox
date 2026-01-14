#ifndef VELOX_CORE_H
#define VELOX_CORE_H

#include "VeloxNeural.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- BITSTREAM ---
class BitStream
{
    std::vector<uint8_t> buffer;
    uint64_t bit_acc = 0;
    int bit_count = 0;
    size_t read_pos = 0;

public:
    BitStream() { buffer.reserve(8 * 1024 * 1024); }
    BitStream(const uint8_t *data, size_t size) : buffer(data, data + size) {}

    inline void WriteBit(int bit)
    {
        if (bit)
            bit_acc |= (1ULL << bit_count);
        bit_count++;
        if (bit_count == 8)
        {
            buffer.push_back(bit_acc & 0xFF);
            bit_acc = 0;
            bit_count = 0;
        }
    }

    inline void Write(uint32_t value, int num_bits)
    {
        for (int i = 0; i < num_bits; i++)
            WriteBit((value >> i) & 1);
    }

    inline void Flush()
    {
        if (bit_count > 0)
            buffer.push_back(bit_acc & 0xFF);
    }
    const std::vector<uint8_t> &GetData() const { return buffer; }

    inline int ReadBit()
    {
        if (bit_count == 0)
        {
            if (read_pos >= buffer.size())
                return 0;
            bit_acc = buffer[read_pos++];
            bit_count = 8;
        }
        int val = bit_acc & 1;
        bit_acc >>= 1;
        bit_count--;
        return val;
    }

    inline uint32_t Read(int num_bits)
    {
        uint32_t val = 0;
        for (int i = 0; i < num_bits; i++)
            if (ReadBit())
                val |= (1 << i);
        return val;
    }

    inline int32_t ReadSigned(int num_bits)
    {
        uint32_t val = Read(num_bits);
        if (val & (1 << (num_bits - 1)))
            return (int32_t)(val - (1 << num_bits));
        return (int32_t)val;
    }
};

class VeloxCodec
{
private:
    static inline uint32_t EncodeZigZag(int32_t n) { return (n << 1) ^ (n >> 31); }
    static inline int32_t DecodeZigZag(uint32_t n) { return (n >> 1) ^ -(n & 1); }

    // --- LPC (Windowed) ---
    static void ComputeLPC(const std::vector<int32_t> &data, int order, std::vector<int> &q_coeffs, int &q_shift)
    {
        if (data.size() < order)
            return;
        std::vector<double> windowed(data.size());
        for (size_t i = 0; i < data.size(); i++)
            windowed[i] = data[i] * (0.5 * (1.0 - std::cos(2.0 * M_PI * i / (data.size() - 1))));

        double autocorr[17];
        for (int i = 0; i <= order; ++i)
        {
            double sum = 0;
            for (size_t j = i; j < data.size(); ++j)
                sum += windowed[j] * windowed[j - i];
            autocorr[i] = sum;
        }

        double a[17][17] = {0};
        double e[17] = {0};
        e[0] = autocorr[0];
        if (e[0] < 1e-9)
        {
            q_shift = 0;
            return;
        }

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

        q_shift = 10;
        for (int i = 1; i <= order; ++i)
            q_coeffs.push_back((int)std::floor(a[i][order] * (1 << q_shift) + 0.5));
    }

    // --- ZRL (Zero Run Length) WRITER ---
    static void WriteZRL(BitStream &bs, uint32_t val, int k, int &zero_run)
    {
        if (val == 0)
        {
            zero_run++;
            return;
        }

        if (zero_run > 0)
        {

            // IMPLEMENT ZRL TRỰC TIẾP:
            while (zero_run > 0)
            {
                bs.WriteBit(0);
                bs.WriteBit(0);
                zero_run--;
            }
        }

        // Ghi giá trị khác 0
        uint32_t q = val >> k;
        uint32_t r = val & ((1 << k) - 1);

        if (q < 32)
        {
            for (uint32_t x = 0; x < q; x++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            if (k > 0)
                bs.Write(r, k);
        }
        else
        {
            for (uint32_t x = 0; x < 32; x++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            bs.Write(val, 32);
        }
    }

    // Hàm thực tế mã hóa (không dùng ZRL phức tạp để đảm bảo Context update đúng)
    static void EncodeSample(BitStream &bs, uint32_t m, int k)
    {
        uint32_t q = m >> k;
        uint32_t r = m & ((1 << k) - 1);
        if (q < 32)
        {
            for (uint32_t x = 0; x < q; x++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            if (k > 0)
                bs.Write(r, k);
        }
        else
        {
            for (uint32_t x = 0; x < 32; x++)
                bs.WriteBit(1);
            bs.WriteBit(0);
            bs.Write(m, 32);
        }
    }

    static uint32_t DecodeSample(BitStream &bs, int k)
    {
        uint32_t q = 0;
        while (bs.ReadBit() == 1)
            q++;
        uint32_t m;
        if (q < 32)
        {
            uint32_t r = (k > 0) ? bs.Read(k) : 0;
            m = (q << k) | r;
        }
        else
        {
            m = bs.Read(32);
        }
        return m;
    }

    // --- PIPELINE XỬ LÝ BLOCK ---
    static void EncodeBlock(const std::vector<int32_t> &data, BitStream &bs, uint32_t global_context)
    {
        size_t n = data.size();
        if (n == 0)
            return;

        // Tầng 1: LPC
        int order = 16;
        std::vector<int> lpc_coeffs;
        int lpc_shift = 0;
        ComputeLPC(data, order, lpc_coeffs, lpc_shift);
        if (lpc_coeffs.size() != order)
        {
            lpc_coeffs.assign(order, 0);
            lpc_shift = 0;
        }

        bs.Write(lpc_shift, 5);
        for (int c : lpc_coeffs)
            bs.Write(c & 0xFFFF, 16);

        // Tầng 2: Deep Neural (Dual-LMS)
        DeepNeuralPredictor neuralNet;
        ContextModeler ctxModel;

        // Load Global Context (Pre-training)
        ctxModel.SetInitialState(global_context);

        for (size_t i = 0; i < n; i++)
        {
            int32_t sample = data[i];

            // LPC Predict
            int64_t sumLPC = 0;
            for (int j = 0; j < order; j++)
            {
                if (i > j)
                    sumLPC += (int64_t)lpc_coeffs[j] * data[i - 1 - j];
            }
            int32_t predLPC = (int32_t)(sumLPC >> lpc_shift);
            int32_t resLPC = sample - predLPC;

            // AI Predict (đoán sai số của LPC)
            int32_t predNeural = neuralNet.Predict();
            int32_t finalRes = resLPC - predNeural;

            // Entropy Encode
            int k = ctxModel.GetK();
            uint32_t m = EncodeZigZag(finalRes);
            EncodeSample(bs, m, k);

            // Learning
            neuralNet.Update(resLPC, predNeural);
            ctxModel.Update(m);
        }
    }

    static std::vector<int32_t> DecodeBlock(BitStream &bs, size_t count, uint32_t global_context)
    {
        std::vector<int32_t> output;
        output.reserve(count);

        int order = 16;
        int lpc_shift = bs.Read(5);
        std::vector<int> lpc_coeffs;
        for (int i = 0; i < order; i++)
            lpc_coeffs.push_back(bs.ReadSigned(16));

        DeepNeuralPredictor neuralNet;
        ContextModeler ctxModel;
        ctxModel.SetInitialState(global_context);

        for (size_t i = 0; i < count; i++)
        {
            int k = ctxModel.GetK();
            uint32_t m = DecodeSample(bs, k);
            int32_t finalRes = DecodeZigZag(m);

            int32_t predNeural = neuralNet.Predict();
            int32_t resLPC = finalRes + predNeural;

            int64_t sumLPC = 0;
            for (int j = 0; j < order; j++)
            {
                if (i > j)
                    sumLPC += (int64_t)lpc_coeffs[j] * output[i - 1 - j];
            }
            int32_t predLPC = (int32_t)(sumLPC >> lpc_shift);
            int32_t sample = resLPC + predLPC;

            output.push_back(sample);

            neuralNet.Update(resLPC, predNeural);
            ctxModel.Update(m);
        }
        return output;
    }

    // Helper: Tính Global Context (Năng lượng trung bình toàn file)
    static uint32_t AnalyzeGlobalContext(const std::vector<int16_t> &pcm)
    {
        uint64_t sum = 0;
        // Lấy mẫu nhanh (stride 10)
        for (size_t i = 0; i < pcm.size(); i += 10)
            sum += std::abs(pcm[i]);
        if (pcm.size() == 0)
            return 256;
        return (uint32_t)(sum / (pcm.size() / 10));
    }

public:
    static std::vector<uint8_t> EncodeStereo(const std::vector<int16_t> &pcm)
    {
        BitStream bs;
        uint32_t num_samples = pcm.size() / 2;
        bs.Write(num_samples, 32);

        // --- GLOBAL HEADER ANALYSIS ---
        // Tính toán đặc tính chung của bài nhạc để thiết lập AI ngay từ đầu
        // Giúp AI không phải học lại từ đầu ở mỗi block, tránh lãng phí bit warm-up.
        uint32_t global_ctx = AnalyzeGlobalContext(pcm);
        bs.Write(global_ctx, 16); // Lưu vào Header (16 bit)

        const int BLOCK_SIZE = 4096;
        for (size_t i = 0; i < num_samples; i += BLOCK_SIZE)
        {
            size_t end = std::min(i + BLOCK_SIZE, (size_t)num_samples);
            size_t len = end - i;

            std::vector<int32_t> L, R;
            L.reserve(len);
            R.reserve(len);
            for (size_t j = 0; j < len; j++)
            {
                L.push_back(pcm[(i + j) * 2]);
                R.push_back(pcm[(i + j) * 2 + 1]);
            }

            // Mid-Side luôn
            std::vector<int32_t> M, S;
            for (size_t j = 0; j < len; j++)
            {
                M.push_back((L[j] + R[j]) >> 1);
                S.push_back(L[j] - R[j]);
            }

            EncodeBlock(M, bs, global_ctx);
            EncodeBlock(S, bs, global_ctx);
        }
        bs.Flush();
        return bs.GetData();
    }

    static std::vector<int16_t> DecodeStereo(const uint8_t *data, size_t size)
    {
        BitStream bs(data, size);
        uint32_t num_samples = bs.Read(32);

        // Đọc Global Header
        uint32_t global_ctx = bs.Read(16);

        std::vector<int16_t> output;
        output.reserve(num_samples * 2);

        const int BLOCK_SIZE = 4096;
        size_t total = 0;
        while (total < num_samples)
        {
            size_t len = std::min((size_t)BLOCK_SIZE, (size_t)(num_samples - total));
            auto M = DecodeBlock(bs, len, global_ctx);
            auto S = DecodeBlock(bs, len, global_ctx);

            for (size_t j = 0; j < len; j++)
            {
                int32_t m = M[j];
                int32_t s = S[j];
                int32_t l = m + ((s + 1) >> 1);
                int32_t r = m - (s >> 1);
                output.push_back((int16_t)l);
                output.push_back((int16_t)r);
            }
            total += len;
        }
        return output;
    }
};

#endif