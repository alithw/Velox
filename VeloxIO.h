#ifndef VELOX_IO_H
#define VELOX_IO_H

#include <string>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdint>

// --- ENDIANNESS UTILS ---
class EndianUtils
{
public:
    static uint32_t Swap32(uint32_t x)
    {
        return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) |
               ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24);
    }
    static uint16_t Swap16(uint16_t x)
    {
        return (x >> 8) | (x << 8);
    }

    static void SwapBuffer24(uint8_t *data, size_t count)
    {
        for (size_t i = 0; i < count; i += 3)
        {
            std::swap(data[i], data[i + 2]);
        }
    }

    // Swap 16-bit buffer (endianness conversion)
    static void SwapBuffer16(uint8_t *data, size_t count)
    {
        uint16_t *ptr = (uint16_t *)data;
        size_t samples = count / 2;
        for (size_t i = 0; i < samples; i++)
            ptr[i] = Swap16(ptr[i]);
    }

    // Swap 32-bit buffer (endianness conversion)
    static void SwapBuffer32(uint8_t *data, size_t count)
    {
        uint32_t *ptr = (uint32_t *)data;
        size_t samples = count / 4;
        for (size_t i = 0; i < samples; i++)
            ptr[i] = Swap32(ptr[i]);
    }
};

struct AudioMetadata
{
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint16_t formatCode; // 1=PCM, 3=Float
    uint32_t dataPos;
    uint32_t dataSize;
    bool isBigEndian; // True for AIFF
};

class AudioLoader
{
public:
    static bool DetectAndParse(const std::string &path, AudioMetadata &meta)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            return false;

        char id[5] = {0};
        f.read(id, 4);

        if (strncmp(id, "RIFF", 4) == 0)
            return ParseWAV(f, meta);
        if (strncmp(id, "FORM", 4) == 0)
            return ParseAIFF(f, meta);

        return false;
    }

private:
    static uint32_t Read32(std::ifstream &f, bool bigEndian)
    {
        uint32_t v;
        f.read((char *)&v, 4);
        return bigEndian ? EndianUtils::Swap32(v) : v;
    }
    static uint16_t Read16(std::ifstream &f, bool bigEndian)
    {
        uint16_t v;
        f.read((char *)&v, 2);
        return bigEndian ? EndianUtils::Swap16(v) : v;
    }

    // WAV Parser (Little Endian)
    static bool ParseWAV(std::ifstream &f, AudioMetadata &meta)
    {
        meta.isBigEndian = false;
        f.seekg(8, std::ios::beg);
        char wave[5] = {0};
        f.read(wave, 4);
        if (strncmp(wave, "WAVE", 4) != 0)
            return false;

        while (f.good())
        {
            char chunkID[5] = {0};
            f.read(chunkID, 4);
            if (f.gcount() < 4)
                break;
            uint32_t size = Read32(f, false);
            uint32_t nextChunk = (uint32_t)f.tellg() + size + (size % 2);

            if (strncmp(chunkID, "fmt ", 4) == 0)
            {
                meta.formatCode = Read16(f, false);
                meta.channels = Read16(f, false);
                meta.sampleRate = Read32(f, false);
                Read32(f, false); // ByteRate
                Read16(f, false); // BlockAlign
                meta.bitsPerSample = Read16(f, false);
            }
            else if (strncmp(chunkID, "data", 4) == 0)
            {
                meta.dataPos = (uint32_t)f.tellg();
                meta.dataSize = size;
                return true; // Found data, ready to go
            }
            f.seekg(nextChunk, std::ios::beg);
        }
        return false;
    }

    // AIFF Parser (Big Endian)
    static bool ParseAIFF(std::ifstream &f, AudioMetadata &meta)
    {
        meta.isBigEndian = true;
        f.seekg(8, std::ios::beg);
        char aiff[5] = {0};
        f.read(aiff, 4);
        if (strncmp(aiff, "AIFF", 4) != 0 && strncmp(aiff, "AIFC", 4) != 0)
            return false;

        while (f.good())
        {
            char chunkID[5] = {0};
            f.read(chunkID, 4);
            if (f.gcount() < 4)
                break;
            uint32_t size = Read32(f, true); // AIFF chunk sizes are Big Endian
            uint32_t nextChunk = (uint32_t)f.tellg() + size + (size % 2);

            if (strncmp(chunkID, "COMM", 4) == 0)
            {
                meta.channels = Read16(f, true);
                Read32(f, true); // numSampleFrames
                meta.bitsPerSample = Read16(f, true);

                // SampleRate in AIFF is 80-bit float (Extended).
                // Simplification: Assume common rates (44100, 48000) stored in exponent/mantissa
                // This is complex. For now, let's skip strict 80-bit parsing and define fixed logic
                // OR read 2 bytes exp + 2 bytes mantissa high.
                // Standard hack:
                unsigned char srate[10];
                f.read((char *)srate, 10);
                // TODO: Implement ieee80_to_double properly.
                // For now, let's rely on user metadata or assume standard if parsing fails?
                // Actually, let's implement a simple parser:
                // (Code 80-bit float parsing is too long here, skipping for brevity, assume 44100/48000 works via context or libraries)
                // WARNING: AIFF sample rate parsing is tricky without external lib.
                // Let's hardcode a placeholder or use a simpler trick if possible.
                // NOTE: For Velox v1.1 prototype, we might assume 44100 if parse fails.
                // *But wait*, let's just use the integer part if possible.

                // Let's use a simplified assumption for common rates:
                // 44100 = 0x400E AC44 ...
                // 48000 = 0x400E BB80 ...
                uint16_t exp = (srate[0] << 8) | srate[1];
                uint64_t mant = 0;
                for (int i = 0; i < 8; i++)
                    mant = (mant << 8) | srate[2 + i];

                if (exp > 16383)
                {
                    meta.sampleRate = (uint32_t)(mant >> (63 - (exp - 16383)));
                }
                else
                    meta.sampleRate = 44100; // Fallback

                meta.formatCode = 1; // AIFF is usually PCM
            }
            else if (strncmp(chunkID, "SSND", 4) == 0)
            {
                uint32_t offset = Read32(f, true);
                uint32_t blockSize = Read32(f, true);
                meta.dataPos = (uint32_t)f.tellg() + offset;
                meta.dataSize = size - 8; // Minus offset/blockSize fields
                return true;
            }
            f.seekg(nextChunk, std::ios::beg);
        }
        return false;
    }
};

#endif