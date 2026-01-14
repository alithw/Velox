#include <vector>
#include <cstdint>
#include <fstream>

class BitWriter {
    std::vector<uint8_t> buffer;
    uint64_t bit_accumulator = 0;
    int bit_count = 0;
public:
    void WriteBits(uint64_t value, int num_bits) {
        // Đẩy bits vào thanh ghi tạm
        bit_accumulator |= (value << bit_count);
        bit_count += num_bits;
        
        // Khi đủ 8 bits (1 byte), ghi vào buffer
        while (bit_count >= 8) {
            buffer.push_back(static_cast<uint8_t>(bit_accumulator & 0xFF));
            bit_accumulator >>= 8;
            bit_count -= 8;
        }
    }
    
    void Flush() {
        if (bit_count > 0) {
            buffer.push_back(static_cast<uint8_t>(bit_accumulator & 0xFF));
            bit_count = 0;
            bit_accumulator = 0;
        }
    }
    
    const std::vector<uint8_t>& GetData() const { return buffer; }
};

class BitReader {
    const uint8_t* data;
    size_t size;
    size_t byte_pos = 0;
    uint64_t bit_accumulator = 0;
    int bit_count = 0;
public:
    BitReader(const uint8_t* in_data, size_t in_size) : data(in_data), size(in_size) {}
    
    uint64_t ReadBits(int num_bits) {
        // Nạp thêm byte nếu thiếu bit
        while (bit_count < num_bits) {
            if (byte_pos >= size) break; // End of stream
            bit_accumulator |= (static_cast<uint64_t>(data[byte_pos]) << bit_count);
            byte_pos++;
            bit_count += 8;
        }
        
        uint64_t result = bit_accumulator & ((1ULL << num_bits) - 1);
        bit_accumulator >>= num_bits;
        bit_count -= num_bits;
        return result;
    }
};