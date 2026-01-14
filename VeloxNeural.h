#ifndef VELOX_NEURAL_H
#define VELOX_NEURAL_H

#include <vector>
#include <cstdint>
#include <cstring>

class DeepNeuralPredictor
{
private:
    static const int LONG_ORDER = 64; // Bộ nhớ dài (Deep Bass)
    static const int SHORT_ORDER = 8; // Bộ nhớ ngắn (Transients)

    int32_t history[LONG_ORDER];

    // Hai bộ trọng số
    int32_t weights_long[LONG_ORDER];
    int32_t weights_short[SHORT_ORDER];

    int shift_long;
    int shift_short;

public:
    DeepNeuralPredictor() { Reset(); }

    void Reset()
    {
        std::memset(history, 0, sizeof(history));
        std::memset(weights_long, 0, sizeof(weights_long));
        std::memset(weights_short, 0, sizeof(weights_short));
        shift_long = 11; // Học rất chậm (độ chính xác cao)
        shift_short = 7; // Học rất nhanh
    }

    inline int32_t Predict()
    {
        int64_t sum_long = 0;
        int64_t sum_short = 0;

        // Slow Brain
        for (int i = 0; i < LONG_ORDER; i++)
            sum_long += (int64_t)history[i] * weights_long[i];

        // Fast Brain
        for (int i = 0; i < SHORT_ORDER; i++)
            sum_short += (int64_t)history[i] * weights_short[i];

        // Kết hợp kết quả (Average)
        int32_t pred1 = (int32_t)(sum_long >> shift_long);
        int32_t pred2 = (int32_t)(sum_short >> shift_short);

        return (pred1 + pred2) >> 1;
    }

    inline void Update(int32_t actual, int32_t predicted)
    {
        int32_t error = actual - predicted;
        int sign = (error > 0) ? 1 : ((error < 0) ? -1 : 0);
        if (sign == 0)
        {
            // Push history even if no error
            for (int i = LONG_ORDER - 1; i > 0; i--)
                history[i] = history[i - 1];
            history[0] = actual;
            return;
        }

        // Cập nhật Fast Brain (Aggressive)
        for (int i = 0; i < SHORT_ORDER; i++)
        {
            int isign = (history[i] > 0) ? 1 : ((history[i] < 0) ? -1 : 0);
            if (sign == isign)
                weights_short[i] += 2; // Tăng nhanh
            else if (isign != 0)
                weights_short[i] -= 2;
        }

        // Cập nhật Slow Brain (Conservative)
        for (int i = 0; i < LONG_ORDER; i++)
        {
            int isign = (history[i] > 0) ? 1 : ((history[i] < 0) ? -1 : 0);
            if (sign == isign)
                weights_long[i] += 1;
            else if (isign != 0)
                weights_long[i] -= 1;

            // Leakage control để tránh overflow
            if (weights_long[i] > 65536)
                weights_long[i] = 65536;
            if (weights_long[i] < -65536)
                weights_long[i] = -65536;
        }

        // Shift History
        for (int i = LONG_ORDER - 1; i > 0; i--)
            history[i] = history[i - 1];
        history[0] = actual;
    }
};

class ContextModeler
{
private:
    uint32_t mean_energy;

public:
    ContextModeler() { mean_energy = 256; } // Start assumption

    // Cho phép thiết lập trạng thái ban đầu từ Header (Pre-trained)
    void SetInitialState(uint32_t init_val)
    {
        if (init_val > 0)
            mean_energy = init_val;
    }

    inline int GetK() const
    {
        if (mean_energy == 0)
            return 0;
        int k = 31 - __builtin_clz(mean_energy);
        if (k < 0)
            k = 0;
        return k;
    }

    inline void Update(uint32_t magnitude)
    {
        mean_energy = mean_energy - (mean_energy >> 2) + (magnitude >> 2);
        if (mean_energy < 1)
            mean_energy = 1;
    }
};

#endif