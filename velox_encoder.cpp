#include "velox_core.h"
#include <immintrin.h> // Thư viện cho AVX/SSE

void VeloxEncoder::ComputeResiduals_AVX2(const int32_t* src, const int32_t* pred, int32_t* dest, int n) {
    int i = 0;
    
    // Xử lý 8 mẫu một lúc bằng AVX2 (256-bit registers)
    // Tốc độ lý thuyết: Nhanh gấp 8 lần vòng lặp thường
    for (; i <= n - 8; i += 8) {
        // Load dữ liệu gốc và dữ liệu dự đoán vào thanh ghi
        __m256i raw_data = _mm256_loadu_si256((__m256i*)&src[i]);
        __m256i pred_data = _mm256_loadu_si256((__m256i*)&pred[i]);

        // Tính hiệu: Residual = Raw - Predicted
        __m256i residual = _mm256_sub_epi32(raw_data, pred_data);

        // Lưu kết quả ra bộ nhớ
        _mm256_storeu_si256((__m256i*)&dest[i], residual);
    }

    // Xử lý các mẫu lẻ còn lại (nếu n không chia hết cho 8)
    for (; i < n; ++i) {
        dest[i] = src[i] - pred[i];
    }
}