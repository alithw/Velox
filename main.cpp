#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <iomanip>
#include "VeloxCore.h"
#include "VeloxMetadata.h"

#pragma pack(push, 1)
struct WavHeaderRaw {
    char riff[4]; uint32_t fileSize; char wave[4];
    char fmt[4]; uint32_t fmtSize; uint16_t audioFormat; uint16_t channels;
    uint32_t sampleRate; uint32_t byteRate; uint16_t blockAlign; uint16_t bitsPerSample;
};
#pragma pack(pop)

std::string GetFileName(const std::string& path) {
    size_t last = path.find_last_of("/\\");
    return (last == std::string::npos) ? path : path.substr(last + 1);
}

bool AnalyzeWav(std::ifstream& f, uint32_t& dPos, uint32_t& dSize, uint32_t& fPos, uint32_t& fSize, 
                uint16_t& fmtCode, uint16_t& bits, uint32_t& sampleRate, uint16_t& channels) {
    f.seekg(0, std::ios::end);
    uint32_t fTotal = (uint32_t)f.tellg();
    f.seekg(0);
    
    WavHeaderRaw h;
    f.read((char*)&h, sizeof(h));
    if (strncmp(h.riff, "RIFF", 4) != 0) return false;
    
    fmtCode = h.audioFormat;
    bits = h.bitsPerSample;
    sampleRate = h.sampleRate;
    channels = h.channels;
    
    f.seekg(12);
    char buf[4];
    while (f.read(buf, 4)) {
        uint32_t cSize;
        f.read((char*)&cSize, 4);
        uint32_t cur = (uint32_t)f.tellg();
        
        if (strncmp(buf, "data", 4) == 0) {
            dPos = cur;
            dSize = cSize;
            fPos = cur + cSize + (cSize % 2); 
            fSize = (fPos < fTotal) ? (fTotal - fPos) : 0;
            return true;
        }
        f.seekg(cSize + (cSize % 2), std::ios::cur);
    }
    return false;
}

int main(int argc, char* argv[]) {
    std::cout << "=== VELOX CODEC ===\n";
    if (argc < 4) {
        std::cout << "Usage:\n";
        std::cout << "  Encode: velox -c input.wav output.vlx [Artist] [Title]\n";
        std::cout << "  Decode: velox -d input.vlx output.wav\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string inF = argv[2];
    std::string outF = argv[3];

    // --- ENCODE MODE ---
    if (mode == "-c") {
        std::string metaArtist = (argc > 4) ? argv[4] : "Unknown Artist";
        std::string metaTitle = (argc > 5) ? argv[5] : GetFileName(inF);

        std::ifstream in(inF, std::ios::binary);
        if (!in.is_open()) { std::cerr << "Error: Cannot open input file.\n"; return 1; }

        uint32_t dPos, dSize, fPos, fSize;
        uint16_t fmtCode, bits, channels;
        uint32_t sampleRate;

        if (!AnalyzeWav(in, dPos, dSize, fPos, fSize, fmtCode, bits, sampleRate, channels)) {
            std::cerr << "Error: Invalid WAV format.\n"; return 1;
        }

        std::cout << "[1] Loading Audio: " << sampleRate << "Hz / " << bits << "bit / " << channels << "ch\n";
        
        // Read raw file
        std::vector<uint8_t> raw(dSize);
        in.seekg(dPos);
        in.read((char*)raw.data(), dSize);

        std::vector<velox_sample_t> samples;
        std::vector<uint8_t> exponents;
        bool isFloat = (fmtCode == 3);

        if (isFloat) {
            FormatHandler::SplitFloat32(raw.data(), dSize / 4, samples, exponents);
        } else {
            FormatHandler::BytesToSamples(raw.data(), dSize / (bits / 8), bits, samples);
        }

        std::cout << "[2] Compressing (LTP + Neural + LPC)...\n";
        
        VeloxCodec::Encoder encoder;
        auto compData = encoder.ProcessBlock(samples, isFloat, exponents);

        // Wrỉte file
        std::ofstream out(outF, std::ios::binary);
        // 1. Header
        VeloxHeader vh = {0x584C4556, 0x0500, sampleRate, channels, bits, fmtCode, (uint64_t)samples.size(), dPos, fSize};
        out.write((char*)&vh, sizeof(vh));
        // 2. Metadata
        VeloxMetadata meta;
        meta.SetTag("ARTIST", metaArtist);
        meta.SetTag("TITLE", metaTitle);
        meta.SetTag("ENCODER", "Velox Beta");
        meta.WriteToStream(out);
        // 3. Raw WAV Header Blob
        std::vector<uint8_t> hData(dPos);
        in.seekg(0);
        in.read((char*)hData.data(), dPos);
        out.write((char*)hData.data(), dPos);
        // 4. Raw WAV Footer Blob
        std::vector<uint8_t> fData(fSize);
        if (fSize > 0) {
            in.seekg(fPos);
            in.read((char*)fData.data(), fSize);
        }
        out.write((char*)fData.data(), fSize);
        // 5. Compressed Stream
        out.write((char*)compData.data(), compData.size());

        float ratio = 100.0f * (float)out.tellp() / (float)(dSize + dPos + fSize);
        std::cout << "Done! Output: " << out.tellp() << " bytes. Ratio: " << std::fixed << std::setprecision(2) << ratio << "%\n";
    }
    
    // --- DECODE MODE ---
    else if (mode == "-d") {
        std::ifstream in(inF, std::ios::binary);
        if (!in.is_open()) { std::cerr << "Error: Cannot open input file.\n"; return 1; }
        VeloxHeader vh;
        in.read((char*)&vh, sizeof(vh));
        if (vh.magic != 0x584C4556) { std::cerr << "Error: Not a Velox file.\n"; return 1; }
        if (vh.version >= 0x0400) {
            VeloxMetadata meta;
            if (meta.ReadFromStream(in)) {
                std::cout << "[Metadata] " << meta.GetTag("TITLE") << " - " << meta.GetTag("ARTIST") << "\n";
            }
        }

        std::vector<uint8_t> hData(vh.header_blob_size);
        in.read((char*)hData.data(), vh.header_blob_size);
        
        std::vector<uint8_t> fData(vh.footer_blob_size);
        in.read((char*)fData.data(), vh.footer_blob_size);

        // Đọc Compressed Data
        std::cout << "[1] Reading Stream...\n";
        std::vector<uint8_t> compData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::cout << "[2] Decoding (Streaming Engine)...\n";
        VeloxCodec::StreamingDecoder decoder(compData.data(), compData.size(), vh.total_samples);
        std::vector<velox_sample_t> outSamples;
        std::vector<uint8_t> outExponents;
        outSamples.resize(vh.total_samples);
        outExponents.resize(vh.total_samples);

        for (size_t i = 0; i < vh.total_samples; i++) {
            if (!decoder.DecodeNext(outSamples[i], outExponents[i])) {
                std::cerr << "Warning: Unexpected End of Stream at sample " << i << "\n";
                break;
            }
        }

        // Reconstruct Raw Bytes
        std::cout << "[3] Reconstructing WAV...\n";
        std::vector<uint8_t> rawBytes;
        bool isFloat = decoder.IsFloat(); 

        if (isFloat) {
            FormatHandler::MergeFloat32(outSamples, outExponents, rawBytes);
        } else {
            FormatHandler::SamplesToBytes(outSamples, vh.bits_per_sample, rawBytes);
        }

        // Ghi file WAV
        std::ofstream out(outF, std::ios::binary);
        out.write((char*)hData.data(), hData.size()); 
        out.write((char*)rawBytes.data(), rawBytes.size()); 
        
        if (rawBytes.size() % 2 != 0) { char z=0; out.write(&z, 1); }
        
        out.write((char*)fData.data(), fData.size()); 

        std::cout << "Done: " << outF << "\n";
    }

    return 0;
}