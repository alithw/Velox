#ifndef VELOX_METADATA_H
#define VELOX_METADATA_H

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cstdint>

// --- HELPER FUNCTIONS FOR ENDIANNESS ---
static void Write32LE(std::vector<uint8_t> &buf, uint32_t val)
{
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

static uint32_t Read32LE(const uint8_t *buf, size_t &offset)
{
    uint32_t val = buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | (buf[offset + 3] << 24);
    offset += 4;
    return val;
}

static void WriteString(std::vector<uint8_t> &buf, const std::string &str)
{
    Write32LE(buf, (uint32_t)str.length());
    for (char c : str)
        buf.push_back(c);
}

static std::string ReadString(const uint8_t *buf, size_t &offset, size_t maxLen)
{
    if (offset + 4 > maxLen)
        return "";
    uint32_t len = Read32LE(buf, offset);
    if (offset + len > maxLen)
        return "";
    std::string s((const char *)(buf + offset), len);
    offset += len;
    return s;
}

// --- VELOX METADATA CLASS (VORBIS STYLE) ---
class VeloxMetadata
{
public:
    std::map<std::string, std::string> tags;

    // Cover Art Data
    struct Picture
    {
        std::string mimeType; // e.g., "image/jpeg"
        std::string description;
        std::vector<uint8_t> data;
    } coverArt;

    bool hasCoverArt = false;

    // --- API ---

    void SetTag(std::string key, std::string value)
    {
        // Vorbis keys are case-insensitive ASCII. Uppercase for consistency.
        std::transform(key.begin(), key.end(), key.begin(), ::toupper);
        tags[key] = value;
    }

    std::string GetTag(std::string key)
    {
        std::transform(key.begin(), key.end(), key.begin(), ::toupper);
        if (tags.find(key) != tags.end())
            return tags[key];
        return "";
    }

    void SetCoverArt(const std::vector<uint8_t> &imageData, const std::string &mime = "image/jpeg")
    {
        coverArt.data = imageData;
        coverArt.mimeType = mime;
        hasCoverArt = !imageData.empty();
    }

    // --- SERIALIZATION (GHI FILE) ---
    void WriteToStream(std::ofstream &out)
    {
        std::vector<uint8_t> block;

        // 1. Vendor String (Required by Vorbis)
        WriteString(block, "Velox Codec v4.0");

        // 2. User Comment List Length
        Write32LE(block, (uint32_t)tags.size());

        // 3. User Comments (KEY=VALUE)
        for (auto const &[key, val] : tags)
        {
            std::string entry = key + "=" + val;
            WriteString(block, entry);
        }

        // 4. Picture Block (Simplified FLAC Picture Block)
        // Flag 1 byte: 1 = Has Picture, 0 = No
        block.push_back(hasCoverArt ? 1 : 0);
        if (hasCoverArt)
        {
            WriteString(block, coverArt.mimeType);
            Write32LE(block, (uint32_t)coverArt.data.size());
            block.insert(block.end(), coverArt.data.begin(), coverArt.data.end());
        }

        // 5. PADDING ALIGNMENT (Sector Alignment 4KB)
        // Header (4 bytes Size) + Block Data
        size_t currentSize = 4 + block.size();
        size_t paddingNeeded = 0;

        // Align to next 4096 bytes
        size_t remainder = currentSize % 4096;
        if (remainder != 0)
        {
            paddingNeeded = 4096 - remainder;
        }
        else
        {
            // Nếu tình cờ vừa khít, thêm hẳn 1 block 4KB để dự phòng edit sau này
            paddingNeeded = 4096;
        }

        // Write Total Size of Metadata Block (bao gồm cả padding data, không tính 4 byte size header)
        uint32_t totalPayloadSize = (uint32_t)(block.size() + paddingNeeded);
        out.write((char *)&totalPayloadSize, 4);

        // Write Data
        out.write((char *)block.data(), block.size());

        // Write Padding Zeros
        if (paddingNeeded > 0)
        {
            std::vector<uint8_t> pads(paddingNeeded, 0);
            out.write((char *)pads.data(), paddingNeeded);
        }
    }

    // --- DESERIALIZATION ---
    bool ReadFromStream(std::ifstream &in)
    {
        tags.clear();
        hasCoverArt = false;

        // Read Block Size
        uint32_t blockSize;
        in.read((char *)&blockSize, 4);
        if (in.gcount() != 4)
            return false;

        std::vector<uint8_t> buffer(blockSize);
        in.read((char *)buffer.data(), blockSize);
        if (in.gcount() != blockSize)
            return false;

        const uint8_t *ptr = buffer.data();
        size_t offset = 0;
        size_t maxLen = blockSize;

        // 1. Vendor
        std::string vendor = ReadString(ptr, offset, maxLen);

        // 2. Comments Count
        if (offset + 4 > maxLen)
            return false;
        uint32_t count = Read32LE(ptr, offset);

        // 3. Parse Comments
        for (uint32_t i = 0; i < count; i++)
        {
            std::string entry = ReadString(ptr, offset, maxLen);
            size_t eqPos = entry.find('=');
            if (eqPos != std::string::npos)
            {
                std::string key = entry.substr(0, eqPos);
                std::string val = entry.substr(eqPos + 1);
                std::transform(key.begin(), key.end(), key.begin(), ::toupper);
                tags[key] = val;
            }
        }

        // 4. Picture
        if (offset < maxLen)
        {
            uint8_t picFlag = ptr[offset++];
            if (picFlag == 1)
            {
                coverArt.mimeType = ReadString(ptr, offset, maxLen);
                if (offset + 4 <= maxLen)
                {
                    uint32_t picLen = Read32LE(ptr, offset);
                    if (offset + picLen <= maxLen)
                    {
                        coverArt.data.assign(ptr + offset, ptr + offset + picLen);
                        hasCoverArt = true;
                        offset += picLen;
                    }
                }
            }
        }

        return true;
    }

    void PrintInfo()
    {
        std::cout << "[Metadata] Vendor: Velox Codec\n";
        for (auto const &[key, val] : tags)
        {
            std::cout << "  " << key << ": " << val << "\n";
        }
        if (hasCoverArt)
        {
            std::cout << "  Cover Art: Yes (" << coverArt.data.size() << " bytes, " << coverArt.mimeType << ")\n";
        }
        else
        {
            std::cout << "  Cover Art: No\n";
        }
    }
};

#endif