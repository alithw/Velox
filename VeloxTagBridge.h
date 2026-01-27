#ifndef VELOX_TAG_BRIDGE_H
#define VELOX_TAG_BRIDGE_H

#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <algorithm>
#include "VeloxMetadata.h"

class TagBridge {
private:
    static uint32_t Read32BE(const uint8_t* b) {
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    }
    
    // ID3v2 SyncSafe Integer
    static uint32_t ReadSyncSafe(const uint8_t* b) {
        return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3];
    }

    // RIFF/WAV Little Endian
    static uint32_t Read32LE(std::ifstream& f) {
        uint32_t v; f.read((char*)&v, 4); return v;
    }

    static void CleanString(std::string& s) {
        s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
    }

public:
    static bool ImportTags(std::string inputPath, VeloxMetadata& outMeta) {
        std::ifstream f(inputPath, std::ios::binary);
        if (!f.is_open()) return false;

        bool found = false;

        // 1. Check ID3v2 (Đầu file)
        char id3[3];
        f.read(id3, 3);
        if (strncmp(id3, "ID3", 3) == 0) {
            f.seekg(0);
            if (ParseID3v2(f, outMeta)) found = true;
        }

        // 2. Check RIFF INFO (WAV)
        f.seekg(0);
        char riff[4]; f.read(riff, 4);
        if (strncmp(riff, "RIFF", 4) == 0) {
            if (ParseRIFFInfo(f, outMeta)) found = true;
        }

        return found;
    }

    static bool ParseID3v2(std::ifstream& f, VeloxMetadata& outMeta) {
        char header[10];
        f.read(header, 10);
        uint32_t size = ReadSyncSafe((uint8_t*)header + 6);
        
        size_t endPos = 10 + size;
        
        while ((size_t)f.tellg() < endPos) {
            char frameHeader[10];
            f.read(frameHeader, 10);
            if (frameHeader[0] == 0) break; 

            std::string fid(frameHeader, 4);
            uint32_t fsize = Read32BE((uint8_t*)frameHeader + 4); 
            
            // --- FIX WARNING TẠI ĐÂY ---
            // Ép kiểu f.tellg() sang size_t để so sánh số học thuần túy
            if (fsize == 0 || (size_t)f.tellg() + fsize > endPos) break;

            std::vector<char> content(fsize);
            f.read(content.data(), fsize);

            std::string val;
            if (fsize > 1) val.assign(content.data() + 1, fsize - 1);
            CleanString(val);

            if (fid == "TIT2") outMeta.SetTag("TITLE", val);
            else if (fid == "TPE1") outMeta.SetTag("ARTIST", val);
            else if (fid == "TALB") outMeta.SetTag("ALBUM", val);
        }
        return true;
    }

    static bool ParseRIFFInfo(std::ifstream& f, VeloxMetadata& outMeta) {
        f.seekg(12); 
        
        while (f.good()) {
            char id[4]; f.read(id, 4);
            if (f.gcount() < 4) break; // Safety check
            
            uint32_t size = Read32LE(f);
            
            // Tính vị trí chunk tiếp theo để nhảy nếu cần
            // (size_t) ép kiểu để tính toán an toàn
            size_t currentPos = (size_t)f.tellg();
            size_t nextChunk = currentPos + size + (size % 2);

            if (strncmp(id, "LIST", 4) == 0) {
                char type[4]; f.read(type, 4);
                if (strncmp(type, "INFO", 4) == 0) {
                    size_t endList = currentPos + size;
                    while ((size_t)f.tellg() < endList) {
                        char subId[4]; f.read(subId, 4);
                        if (f.gcount() < 4) break;
                        
                        uint32_t subSize = Read32LE(f);
                        
                        std::vector<char> buf(subSize);
                        f.read(buf.data(), subSize);
                        std::string val(buf.begin(), buf.end());
                        CleanString(val);

                        if (strncmp(subId, "INAM", 4) == 0) outMeta.SetTag("TITLE", val);
                        else if (strncmp(subId, "IART", 4) == 0) outMeta.SetTag("ARTIST", val);
                        else if (strncmp(subId, "IPRD", 4) == 0) outMeta.SetTag("ALBUM", val);
                        
                        if (subSize % 2 != 0) f.seekg(1, std::ios::cur); 
                    }
                    return true;
                }
            }
            
            f.seekg(nextChunk, std::ios::beg); 
        }
        return false;
    }
};

#endif