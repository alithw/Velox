#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include "VeloxCore.h"

struct WavStructure {
    uint32_t dataPos;
    uint32_t dataSize;
    uint32_t footerPos;
    uint32_t fileSize;
};

bool AnalyzeWav(std::ifstream& file, WavStructure& info) {
    file.seekg(0, std::ios::end);
    info.fileSize = (uint32_t)file.tellg();
    file.seekg(0, std::ios::beg);

    char buffer[4];
    file.read(buffer, 4);
    if (strncmp(buffer, "RIFF", 4) != 0) return false;
    file.seekg(8, std::ios::cur); // Skip size & WAVE

    while (file.read(buffer, 4)) {
        uint32_t chunkSize;
        file.read((char*)&chunkSize, 4);
        uint32_t currentPos = (uint32_t)file.tellg(); // Vị trí đầu payload của chunk
        
        uint32_t padding = (chunkSize % 2); 
        uint32_t nextChunkHeader = currentPos + chunkSize + padding;

        if (strncmp(buffer, "data", 4) == 0) {
            info.dataPos = currentPos;
            info.dataSize = chunkSize;
            info.footerPos = nextChunkHeader;
            return true;
        }
        file.seekg(nextChunkHeader, std::ios::beg);
        if (file.fail()) break;
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cout << "Velox v0.7 (Safe Mode)\nUsage: velox -c in.wav out.vlx | velox -d in.vlx out.wav\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string inFile = argv[2];
    std::string outFile = argv[3];

    if (mode == "-c") {
        std::ifstream in(inFile, std::ios::binary);
        WavStructure wav;
        if (!AnalyzeWav(in, wav)) { std::cerr << "Invalid WAV.\n"; return 1; }

        // Đọc PCM
        in.seekg(wav.dataPos, std::ios::beg);
        std::vector<int16_t> pcm(wav.dataSize / 2);
        in.read((char*)pcm.data(), wav.dataSize); // Đọc đúng số byte chẵn của PCM

        // Header: Copy mọi thứ từ 0 -> wav.dataPos (bao gồm cả "data" tag và size)
        std::vector<uint8_t> header(wav.dataPos);
        in.seekg(0, std::ios::beg);
        in.read((char*)header.data(), wav.dataPos);

        // Footer: Từ sau data (+padding) đến hết
        std::vector<uint8_t> footer;
        if (wav.footerPos < wav.fileSize) {
            footer.resize(wav.fileSize - wav.footerPos);
            in.seekg(wav.footerPos, std::ios::beg);
            in.read((char*)footer.data(), footer.size());
        }

        // Kiểm tra xem Data chunk gốc có padding byte không (nếu size lẻ)
        uint8_t hasPadding = (wav.dataSize % 2);

        // Nén
        auto compressed = VeloxCodec::EncodeStereo(pcm);

        std::ofstream out(outFile, std::ios::binary);
        out.write("VELX", 4);
        
        uint32_t s;
        s = header.size(); out.write((char*)&s, 4); out.write((char*)header.data(), s);
        s = footer.size(); out.write((char*)&s, 4); out.write((char*)footer.data(), s);
        
        out.write((char*)&hasPadding, 1);
        out.write((char*)compressed.data(), compressed.size());

        std::cout << "Encoded. Audio: " << compressed.size() << " bytes.\n";
    } 
    else if (mode == "-d") {
        std::ifstream in(inFile, std::ios::binary);
        char magic[4]; in.read(magic, 4);

        uint32_t s;
        in.read((char*)&s, 4); std::vector<uint8_t> header(s); in.read((char*)header.data(), s);
        in.read((char*)&s, 4); std::vector<uint8_t> footer(s); in.read((char*)footer.data(), s);
        
        uint8_t hasPadding; in.read((char*)&hasPadding, 1);

        std::vector<uint8_t> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto pcm = VeloxCodec::DecodeStereo(comp.data(), comp.size());

        std::ofstream out(outFile, std::ios::binary);
        // 1. Header
        out.write((char*)header.data(), header.size());
        // 2. Audio Data
        out.write((char*)pcm.data(), pcm.size() * 2);
        // 3. Padding Byte (nếu file gốc có)
        if (hasPadding) { char zero=0; out.write(&zero, 1); }
        // 4. Footer
        out.write((char*)footer.data(), footer.size());

        std::cout << "Restored: " << outFile << "\n";
    }
    return 0;
}