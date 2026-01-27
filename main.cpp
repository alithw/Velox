#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <iomanip>

#include "VeloxCore.h"
#include "VeloxMetadata.h"
#include "VeloxIO.h"
#include "VeloxTagBridge.h"

// --- WAV HEADER GENERATOR ---
// Generate standard WAV header (44 bytes)
std::vector<uint8_t> GenerateWavHeader(uint32_t sampleRate, uint16_t channels, uint16_t bits, uint32_t dataSize, bool isFloat)
{
    std::vector<uint8_t> h(44);
    uint32_t byteRate = sampleRate * channels * (bits / 8);
    uint16_t blockAlign = channels * (bits / 8);
    uint16_t format = isFloat ? 3 : 1;
    uint32_t totalSize = dataSize + 36;

    memcpy(&h[0], "RIFF", 4);
    memcpy(&h[4], &totalSize, 4);
    memcpy(&h[8], "WAVE", 4);
    memcpy(&h[12], "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(&h[16], &fmtSize, 4);
    memcpy(&h[20], &format, 2);
    memcpy(&h[22], &channels, 2);
    memcpy(&h[24], &sampleRate, 4);
    memcpy(&h[28], &byteRate, 4);
    memcpy(&h[32], &blockAlign, 2);
    memcpy(&h[34], &bits, 2);
    memcpy(&h[36], "data", 4);
    memcpy(&h[40], &dataSize, 4);

    return h;
}

std::string GetFileName(const std::string &path)
{
    size_t last = path.find_last_of("/\\");
    return (last == std::string::npos) ? path : path.substr(last + 1);
}

int main(int argc, char *argv[])
{
    std::cout << "=== VELOX CODEC v1.1 (Universal) ===\n";
    if (argc < 4)
    {
        std::cout << "Usage:\n";
        std::cout << "  Encode: velox -c input.wav/aif output.vlx [Artist] [Title]\n";
        std::cout << "  Decode: velox -d input.vlx output.wav\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string inF = argv[2];
    std::string outF = argv[3];

    // --- ENCODE MODE ---
    if (mode == "-c")
    {
        std::string metaArtist = "Unknown Artist";
        std::string metaTitle = GetFileName(inF);
        bool userProvidedTags = false;

        if (argc > 4)
        {
            metaArtist = argv[4];
            userProvidedTags = true;
        }
        if (argc > 5)
        {
            metaTitle = argv[5];
            userProvidedTags = true;
        }

        // 1. Analyze input file (WAV/AIFF)
        AudioMetadata metaInfo;
        if (!AudioLoader::DetectAndParse(inF, metaInfo))
        {
            std::cerr << "Error: Unsupported format or invalid file.\n";
            return 1;
        }

        std::cout << "[1] Loading Audio: " << metaInfo.sampleRate << "Hz / " << metaInfo.bitsPerSample << "bit";
        if (metaInfo.isBigEndian)
            std::cout << " (AIFF)";
        std::cout << "\n";

        // 2. Auto-import tags if user didn't provide them
        if (!userProvidedTags)
        {
            VeloxMetadata importedMeta;
            if (TagBridge::ImportTags(inF, importedMeta))
            {
                std::string a = importedMeta.GetTag("ARTIST");
                std::string t = importedMeta.GetTag("TITLE");
                if (!a.empty())
                    metaArtist = a;
                if (!t.empty())
                    metaTitle = t;
                std::cout << "    -> Auto-Tag: " << metaTitle << " by " << metaArtist << "\n";
            }
        }

        // 3. Read raw PCM data
        std::ifstream in(inF, std::ios::binary);
        std::vector<uint8_t> raw(metaInfo.dataSize);
        in.seekg(metaInfo.dataPos);
        in.read((char *)raw.data(), metaInfo.dataSize);

        // 4. Handle endianness (if AIFF)
        if (metaInfo.isBigEndian)
        {
            if (metaInfo.bitsPerSample == 16)
                EndianUtils::SwapBuffer16(raw.data(), raw.size());
            else if (metaInfo.bitsPerSample == 24)
                EndianUtils::SwapBuffer24(raw.data(), raw.size());
            else if (metaInfo.bitsPerSample == 32)
                EndianUtils::SwapBuffer32(raw.data(), raw.size());
        }

        // 5. Prepare compression
        std::vector<velox_sample_t> samples;
        std::vector<uint8_t> exponents;
        bool isFloat = (metaInfo.formatCode == 3);

        if (isFloat)
            FormatHandler::SplitFloat32(raw.data(), raw.size() / 4, samples, exponents);
        else
            FormatHandler::BytesToSamples(raw.data(), raw.size() / (metaInfo.bitsPerSample / 8), metaInfo.bitsPerSample, samples);

        std::cout << "[2] Compressing...\n";
        VeloxCodec::Encoder encoder;
        auto compData = encoder.ProcessBlock(samples, isFloat, exponents, raw.data());

        // 6. Write .VLX file
        std::ofstream out(outF, std::ios::binary);

        // Header
        bool hasPadding = (raw.size() % 2 != 0);
        uint16_t bits_flag = metaInfo.bitsPerSample;
        if (hasPadding)
            bits_flag |= 0x8000;

        // Handle header blob:
        // - If WAV: Copy original header to preserve unknown metadata.
        // - If AIFF: Generate new WAV header (Velox decompresses to WAV, not AIFF).
        std::vector<uint8_t> headerBlob;
        if (metaInfo.isBigEndian)
        {
            headerBlob = GenerateWavHeader(metaInfo.sampleRate, metaInfo.channels, metaInfo.bitsPerSample, metaInfo.dataSize, isFloat);
        }
        else
        {
            headerBlob.resize(metaInfo.dataPos);
            in.seekg(0);
            in.read((char *)headerBlob.data(), metaInfo.dataPos);
        }

        std::vector<uint8_t> footerBlob;

        if (!metaInfo.isBigEndian)
        { // Only WAV files have footer blocks to preserve
            // Calculate footer start position
            // dataPos + dataSize + padding
            uint32_t footerStart = metaInfo.dataPos + metaInfo.dataSize + (metaInfo.dataSize % 2);

            // Calculate original file size
            in.seekg(0, std::ios::end);
            uint32_t fileSize = (uint32_t)in.tellg();

            if (fileSize > footerStart)
            {
                uint32_t footerLen = fileSize - footerStart;
                footerBlob.resize(footerLen);
                in.seekg(footerStart);
                in.read((char *)footerBlob.data(), footerLen);
            }
        }

        // Update header with actual footer size
        VeloxHeader vh = {
            0x584C4556, 0x0800,
            metaInfo.sampleRate, metaInfo.channels,
            bits_flag, metaInfo.formatCode,
            (uint64_t)samples.size(),
            (uint32_t)headerBlob.size(),
            (uint32_t)footerBlob.size()};
        out.write((char *)&vh, sizeof(vh));

        // Metadata Block
        VeloxMetadata meta;
        meta.SetTag("ARTIST", metaArtist);
        meta.SetTag("TITLE", metaTitle);
        meta.SetTag("ENCODER", "Velox v1.1");
        meta.WriteToStream(out);

        // Raw Header Blob
        out.write((char *)headerBlob.data(), headerBlob.size());
        // Footer Blob
        out.write((char *)footerBlob.data(), footerBlob.size());

        // Compressed Data
        out.write((char *)compData.data(), compData.size());

        float ratio = 100.0f * (float)out.tellp() / (float)(metaInfo.dataSize + headerBlob.size());
        std::cout << "Done! Ratio: " << std::fixed << std::setprecision(2) << ratio << "%\n";
    }

    // --- DECODE MODE ---
    else if (mode == "-d")
    {
        std::ifstream in(inF, std::ios::binary);
        if (!in.is_open())
        {
            std::cerr << "Error open input\n";
            return 1;
        }

        VeloxHeader vh;
        in.read((char *)&vh, sizeof(vh));
        if (vh.magic != 0x584C4556)
        {
            std::cerr << "Invalid File\n";
            return 1;
        }

        bool hasPadding = (vh.bits_per_sample & 0x8000) != 0;
        uint16_t realBits = vh.bits_per_sample & 0x7FFF;

        if (vh.version >= 0x0400)
        {
            VeloxMetadata meta;
            if (meta.ReadFromStream(in))
            {
                std::cout << "[Metadata] " << meta.GetTag("TITLE") << " - " << meta.GetTag("ARTIST") << "\n";
            }
        }

        std::vector<uint8_t> hData(vh.header_blob_size);
        in.read((char *)hData.data(), vh.header_blob_size);

        // Read Footer
        std::vector<uint8_t> fData(vh.footer_blob_size);
        in.read((char *)fData.data(), vh.footer_blob_size);

        std::cout << "[2] Decoding...\n";
        std::vector<uint8_t> compData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        VeloxCodec::StreamingDecoder decoder(compData.data(), compData.size(), vh.total_samples);
        std::vector<velox_sample_t> outSamples(vh.total_samples);
        std::vector<uint8_t> outExponents(vh.total_samples);

        for (size_t i = 0; i < vh.total_samples; i++)
        {
            if (!decoder.DecodeNext(outSamples[i], outExponents[i]))
                break;
        }

        std::cout << "[3] Writing WAV...\n";
        std::vector<uint8_t> rawBytes;

        // Auto-promote logic
        if (decoder.IsFloat())
        {
            FormatHandler::MergeFloat32(outSamples, outExponents, rawBytes);
        }
        else
        {
            if (vh.format_code == 3)
            { // Pseudo-float handling
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
        // Write header (original header or header generated during compression)
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