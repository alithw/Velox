#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include "VeloxCore.h"

#pragma pack(push, 1)
struct WavHeaderRaw {
    char riff[4]; uint32_t fileSize; char wave[4];
    char fmt[4]; uint32_t fmtSize; uint16_t audioFormat; uint16_t channels;
    uint32_t sampleRate; uint32_t byteRate; uint16_t blockAlign; uint16_t bitsPerSample;
};
#pragma pack(pop)

bool AnalyzeWav(std::ifstream& f, uint32_t& dPos, uint32_t& dSize, uint32_t& fPos, uint32_t& fSize, 
                uint16_t& fmtCode, uint16_t& bits, uint32_t& sampleRate, uint16_t& channels) {
    f.seekg(0, std::ios::end);
    uint32_t fTotal = (uint32_t)f.tellg();
    f.seekg(0);
    
    WavHeaderRaw h;
    f.read((char*)&h, sizeof(h));
    if(strncmp(h.riff,"RIFF",4)!=0) return false;
    
    fmtCode = h.audioFormat;
    bits = h.bitsPerSample;
    // --- LẤY THÔNG TIN QUAN TRỌNG ---
    sampleRate = h.sampleRate;
    channels = h.channels;
    
    // Scan chunks
    f.seekg(12);
    char buf[4];
    while(f.read(buf, 4)) {
        uint32_t cSize; f.read((char*)&cSize, 4);
        uint32_t cur = (uint32_t)f.tellg();
        if(strncmp(buf, "data", 4)==0) {
            dPos = cur;
            dSize = cSize;
            fPos = cur + cSize + (cSize%2);
            fSize = (fPos < fTotal) ? (fTotal - fPos) : 0;
            return true;
        }
        f.seekg(cSize + (cSize%2), std::ios::cur);
    }
    return false;
}

int main(int argc, char* argv[]) {
    if(argc < 4) { std::cout << "Usage: velox -c in.wav out.vlx | velox -d in.vlx out.wav\n"; return 1; }
    
    std::string mode = argv[1];
    std::string inF = argv[2];
    std::string outF = argv[3];

    if(mode == "-c") {
        std::ifstream in(inF, std::ios::binary);
        uint32_t dPos, dSize, fPos, fSize;
        uint16_t fmtCode, bits, channels;
        uint32_t sampleRate;
        
        // Gọi hàm phân tích mới
        if(!AnalyzeWav(in, dPos, dSize, fPos, fSize, fmtCode, bits, sampleRate, channels)) { 
            std::cout << "Wav Error\n"; return 1; 
        }

        std::vector<uint8_t> raw(dSize);
        in.seekg(dPos); in.read((char*)raw.data(), dSize);

        std::vector<velox_sample_t> samples;
        std::vector<uint8_t> exponents;
        bool isFloat = (fmtCode == 3);

        if(isFloat) FormatHandler::SplitFloat32(raw.data(), dSize/4, samples, exponents);
        else FormatHandler::BytesToSamples(raw.data(), dSize/(bits/8), bits, samples);

        // Decorrelate (Mid/Side) - GIỮ LẠI ĐỂ ĐẢM BẢO TỶ LỆ NÉN TỐT NHƯ VỪA RỒI
        // Codec v2.0 đã tự làm Adaptive, nhưng logic M/S tĩnh này giúp đảm bảo 
        // dữ liệu đầu vào sạch cho Neural ở bước đầu.
        // Tuy nhiên, để đúng chuẩn v2.0 Adaptive, ta nên để Codec tự quyết.
        // Nhưng logic main cũ bạn chạy ra 60-80% là do Codec tự làm.
        // Ở đây ta KHÔNG can thiệp tay nữa.

        auto comp = VeloxCodec::EncodeBlock(samples, isFloat, exponents);

        std::ofstream out(outF, std::ios::binary);
        
        // --- SỬA LỖI TẠI ĐÂY: Ghi đúng SampleRate và Channels ---
        VeloxHeader vh = {0x584C4556, 0x0200, sampleRate, channels, (uint16_t)bits, fmtCode, (uint64_t)samples.size(), dPos, fSize};
        
        out.write((char*)&vh, sizeof(vh));

        std::vector<uint8_t> hData(dPos);
        in.seekg(0); in.read((char*)hData.data(), dPos);
        out.write((char*)hData.data(), dPos);

        std::vector<uint8_t> fData(fSize);
        if(fSize > 0) { in.seekg(fPos); in.read((char*)fData.data(), fSize); }
        out.write((char*)fData.data(), fSize);
        out.write((char*)comp.data(), comp.size());
        
        std::cout << "Encoded: " << comp.size() << " bytes (" << (100.0f*comp.size()/dSize) << "%)\n";

    } else if(mode == "-d") {
        std::ifstream in(inF, std::ios::binary);
        VeloxHeader vh;
        in.read((char*)&vh, sizeof(vh));

        std::vector<uint8_t> hData(vh.header_blob_size);
        in.read((char*)hData.data(), vh.header_blob_size);
        std::vector<uint8_t> fData(vh.footer_blob_size);
        in.read((char*)fData.data(), vh.footer_blob_size);

        std::vector<uint8_t> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        std::vector<velox_sample_t> samples;
        std::vector<uint8_t> exponents;
        bool isFloat;
        VeloxCodec::DecodeBlock(comp.data(), comp.size(), vh.total_samples, samples, exponents, isFloat);

        std::vector<uint8_t> raw;
        if(isFloat) FormatHandler::MergeFloat32(samples, exponents, raw);
        else FormatHandler::SamplesToBytes(samples, vh.bits_per_sample, raw);

        std::ofstream out(outF, std::ios::binary);
        out.write((char*)hData.data(), hData.size());
        out.write((char*)raw.data(), raw.size());
        if(raw.size()%2 != 0) { char z=0; out.write(&z,1); }
        out.write((char*)fData.data(), fData.size());
        
        std::cout << "Decoded: " << outF << "\n";
    }
    return 0;
}