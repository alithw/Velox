#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <iomanip>
#include "VeloxCore.h"
#include "VeloxMetadata.h"

// --- WAV PARSER MẠNH MẼ ---
// Không dùng struct cố định nữa, mà parse động từng chunk

std::string GetFileName(const std::string &path)
{
    size_t last = path.find_last_of("/\\");
    return (last == std::string::npos) ? path : path.substr(last + 1);
}

// Hàm đọc 4 byte Little Endian an toàn
uint32_t Read32(std::ifstream &f)
{
    uint32_t v;
    f.read((char *)&v, 4);
    return v;
}
uint16_t Read16(std::ifstream &f)
{
    uint16_t v;
    f.read((char *)&v, 2);
    return v;
}

bool AnalyzeWav(std::ifstream &f, uint32_t &dPos, uint32_t &dSize, uint32_t &fPos, uint32_t &fSize,
                uint16_t &fmtCode, uint16_t &bits, uint32_t &sampleRate, uint16_t &channels, bool &hasPadding)
{
    f.seekg(0, std::ios::end);
    uint32_t fTotal = (uint32_t)f.tellg();
    f.seekg(0);

    char id[5] = {0};
    f.read(id, 4);
    if (strcmp(id, "RIFF") != 0)
        return false;
    Read32(f); // Skip RIFF size
    f.read(id, 4);
    if (strcmp(id, "WAVE") != 0)
        return false;

    bool foundFmt = false;
    bool foundData = false;

    // Loop tìm chunk
    while (f.tellg() < fTotal)
    {
        f.read(id, 4);
        if (f.gcount() < 4)
            break;
        uint32_t chunkSize = Read32(f);
        uint32_t chunkStart = (uint32_t)f.tellg();

        if (strcmp(id, "fmt ") == 0)
        {
            fmtCode = Read16(f);
            channels = Read16(f);
            sampleRate = Read32(f);
            Read32(f); // ByteRate
            Read16(f); // BlockAlign
            bits = Read16(f);
            foundFmt = true;
        }
        else if (strcmp(id, "data") == 0)
        {
            dPos = chunkStart;
            dSize = chunkSize;
            hasPadding = (chunkSize % 2 != 0);
            fPos = dPos + dSize + (hasPadding ? 1 : 0);
            fSize = (fPos < fTotal) ? (fTotal - fPos) : 0;
            foundData = true;

            // Nếu đã tìm thấy cả fmt và data thì có thể dừng (hoặc quét tiếp nếu data nằm trước fmt - hiếm gặp)
            if (foundFmt)
                break;
        }

        // Nhảy đến chunk tiếp theo
        // Lưu ý: Chunk size trong WAV luôn được pad chẵn
        uint32_t nextPos = chunkStart + chunkSize + (chunkSize % 2);
        f.seekg(nextPos);
    }

    return foundFmt && foundData;
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        std::cout << "Usage: velox -c in.wav out.vlx [Artist] [Title] | velox -d in.vlx out.wav\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string inF = argv[2];
    std::string outF = argv[3];

    // --- ENCODE ---
    if (mode == "-c")
    {
        std::string metaArtist = (argc > 4) ? argv[4] : "Unknown Artist";
        std::string metaTitle = (argc > 5) ? argv[5] : GetFileName(inF);

        std::ifstream in(inF, std::ios::binary);
        if (!in.is_open())
            return 1;

        uint32_t dPos, dSize, fPos, fSize;
        uint16_t fmtCode, bits, channels;
        uint32_t sampleRate;
        bool hasPadding = false;

        if (!AnalyzeWav(in, dPos, dSize, fPos, fSize, fmtCode, bits, sampleRate, channels, hasPadding))
        {
            std::cout << "Error: Invalid/Unsupported WAV Structure\n";
            return 1;
        }

        std::cout << "[1] Loading Audio: " << sampleRate << "Hz / " << bits << "bit / " << channels << "ch\n";

        std::vector<uint8_t> raw(dSize);
        in.seekg(dPos);
        in.read((char *)raw.data(), dSize);

        std::vector<velox_sample_t> samples;
        std::vector<uint8_t> exponents;
        bool isFloat = (fmtCode == 3);

        if (isFloat)
            FormatHandler::SplitFloat32(raw.data(), dSize / 4, samples, exponents);
        else
            FormatHandler::BytesToSamples(raw.data(), dSize / (bits / 8), bits, samples);

        std::cout << "[2] Compressing (Neural-Parallel)...\n";
        VeloxCodec::Encoder encoder;
        auto compData = encoder.ProcessBlock(samples, isFloat, exponents, raw.data());

        std::ofstream out(outF, std::ios::binary);

        uint16_t bits_flag = bits;
        if (hasPadding)
            bits_flag |= 0x8000;

        VeloxHeader vh = {0x584C4556, 0x0800, sampleRate, channels, bits_flag, fmtCode, (uint64_t)samples.size(), dPos, fSize};
        out.write((char *)&vh, sizeof(vh));

        VeloxMetadata meta;
        meta.SetTag("ARTIST", metaArtist);
        meta.SetTag("TITLE", metaTitle);
        meta.SetTag("ENCODER", "Velox v8.0");
        meta.WriteToStream(out);

        // Header Blob (Copy từ 0 đến dPos)
        std::vector<uint8_t> hData(dPos);
        in.seekg(0);
        in.read((char *)hData.data(), dPos);
        out.write((char *)hData.data(), dPos);

        // Footer Blob
        std::vector<uint8_t> fData(fSize);
        if (fSize > 0)
        {
            in.seekg(fPos);
            in.read((char *)fData.data(), fSize);
        }
        out.write((char *)fData.data(), fSize);

        out.write((char *)compData.data(), compData.size());

        float ratio = 100.0f * (float)out.tellp() / (float)(dSize + dPos + fSize);
        std::cout << "Done! Ratio: " << std::fixed << std::setprecision(2) << ratio << "%\n";
    }

    // --- DECODE ---
    else if (mode == "-d")
    {
        std::ifstream in(inF, std::ios::binary);
        if (!in.is_open())
            return 1;

        VeloxHeader vh;
        in.read((char *)&vh, sizeof(vh));
        if (vh.magic != 0x584C4556)
            return 1;

        bool hasPadding = (vh.bits_per_sample & 0x8000) != 0;
        uint16_t realBits = vh.bits_per_sample & 0x7FFF;

        if (vh.version >= 0x0400)
        {
            VeloxMetadata meta;
            meta.ReadFromStream(in);
        }

        std::vector<uint8_t> hData(vh.header_blob_size);
        in.read((char *)hData.data(), vh.header_blob_size);
        std::vector<uint8_t> fData(vh.footer_blob_size);
        in.read((char *)fData.data(), vh.footer_blob_size);
        std::vector<uint8_t> compData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        std::cout << "[2] Decoding...\n";
        VeloxCodec::StreamingDecoder decoder(compData.data(), compData.size(), vh.total_samples);
        std::vector<velox_sample_t> outSamples(vh.total_samples);
        std::vector<uint8_t> outExponents(vh.total_samples);

        for (size_t i = 0; i < vh.total_samples; i++)
        {
            if (!decoder.DecodeNext(outSamples[i], outExponents[i]))
                break;
        }

        std::cout << "[3] Reconstructing...\n";
        std::vector<uint8_t> rawBytes;

        // Auto-promote logic
        if (decoder.IsFloat())
        {
            FormatHandler::MergeFloat32(outSamples, outExponents, rawBytes);
        }
        else
        {
            // Check if it was pseudo-float (Header says float, decoder says int)
            if (vh.format_code == 3)
            {
                int fMode = decoder.GetFloatMode();
                if (fMode == 1)
                    FormatHandler::PromoteIntToFloat(outSamples, 16, rawBytes);
                else if (fMode == 2)
                    FormatHandler::PromoteIntToFloat(outSamples, 24, rawBytes);
                else
                    FormatHandler::SamplesToBytes(outSamples, realBits, rawBytes);
            }
            else
            {
                FormatHandler::SamplesToBytes(outSamples, realBits, rawBytes);
            }
        }

        std::ofstream out(outF, std::ios::binary);
        out.write((char *)hData.data(), hData.size());
        out.write((char *)rawBytes.data(), rawBytes.size());
        if (hasPadding)
        {
            char z = 0;
            out.write(&z, 1);
        }
        out.write((char *)fData.data(), fData.size());
        std::cout << "Done: " << outF << "\n";
    }
    return 0;
}