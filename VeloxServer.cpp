#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <fstream>
#include <map>

#pragma comment(lib, "Ws2_32.lib")

#define SERVER_PORT "6781"

// --- DATABASE ---
struct TrackInfo
{
    std::string filepath;
    std::string filename;
    uint32_t fileSize;
};

std::vector<TrackInfo> database;

void Log(std::string msg)
{
    std::cout << "[Server] " << msg << std::endl;
}

void BuildDatabase()
{
    Log("Scanning ./music/ directory...");
    CreateDirectoryA("music", NULL);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("music\\*.vlx", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        Log("No .vlx files found in ./music/. Please add some files.");
        return;
    }

    do
    {
        std::string path = std::string("music\\") + fd.cFileName;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open())
            continue;

        TrackInfo track;
        track.filepath = path;
        track.filename = fd.cFileName;
        track.fileSize = (uint32_t)in.tellg(); // Get total file size
        database.push_back(track);

        Log("Hosted: " + track.filename + " (" + std::to_string(track.fileSize / 1024) + " KB)");
        in.close();
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    Log("Database ready. Total tracks: " + std::to_string(database.size()));
}

// Safe data transmission over TCP
bool SendData(SOCKET s, const uint8_t *data, uint32_t size)
{
    uint32_t netSize = htonl(size);
    // 1. Send data block size (4 bytes)
    if (send(s, (char *)&netSize, 4, 0) == SOCKET_ERROR)
        return false;

    // 2. Send Payload
    uint32_t sent = 0;
    while (sent < size)
    {
        int res = send(s, (char *)data + sent, size - sent, 0);
        if (res <= 0)
            return false; // Client disconnected
        sent += res;
    }
    return true;
}

// Handle each client independently
void HandleClient(SOCKET clientSocket)
{
    Log("Client connected. Socket ID: " + std::to_string((int)clientSocket));

    DWORD timeout = 5000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    while (true)
    {
        char cmdBuf[256] = {0};
        int bytes = recv(clientSocket, cmdBuf, 255, 0);

        if (bytes == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
                continue;
            break;
        }
        if (bytes == 0)
            break;

        std::string req(cmdBuf);

        // Command 1: Get music list
        if (req.find("LIST") == 0)
        {
            std::string res = "";
            for (size_t i = 0; i < database.size(); i++)
            {
                res += std::to_string(i) + "|" + database[i].filename + "|" + std::to_string(database[i].fileSize) + "\n";
            }
            if (!SendData(clientSocket, (uint8_t *)res.c_str(), (uint32_t)res.length()))
                break;
        }

        // Command 2: Streaming & Seeking (Range Request)
        // Syntax: GET <Track_ID> <Offset_Byte> <Length_Byte>
        else if (req.find("GET ") == 0)
        {
            int id;
            uint32_t offset, length;
            if (sscanf(req.c_str(), "GET %d %u %u", &id, &offset, &length) == 3)
            {

                if (id >= 0 && id < database.size())
                {
                    TrackInfo &track = database[id];
                    if (offset >= track.fileSize)
                    {
                        SendData(clientSocket, nullptr, 0);
                        continue;
                    }

                    if (offset + length > track.fileSize)
                    {
                        length = track.fileSize - offset;
                    }

                    // Read from disk
                    std::ifstream in(track.filepath, std::ios::binary);
                    if (in.is_open())
                    {
                        in.seekg(offset, std::ios::beg);
                        std::vector<uint8_t> buffer(length);
                        in.read((char *)buffer.data(), length);
                        uint32_t actualRead = (uint32_t)in.gcount();

                        // Send to client
                        if (!SendData(clientSocket, buffer.data(), actualRead))
                            break;
                    }
                    else
                    {
                        SendData(clientSocket, nullptr, 0);
                    }
                }
                else
                {
                    SendData(clientSocket, nullptr, 0);
                }
            }
        }
    }
    closesocket(clientSocket);
    Log("Client disconnected.");
}

int main()
{
    std::cout << "======================================\n";
    std::cout << "|   VELOX STREAMING SERVER           | \n";
    std::cout << "|   - Smart Range Request enabled    | \n";
    std::cout << "|   - Port: " << SERVER_PORT << "    |        \n";
    std::cout << "======================================\n\n";

    BuildDatabase();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Log("WSAStartup failed!");
        return 1;
    }

    struct addrinfo *result = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, SERVER_PORT, &hints, &result) != 0)
    {
        Log("getaddrinfo failed!");
        WSACleanup();
        return 1;
    }

    SOCKET ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ListenSocket == INVALID_SOCKET)
    {
        Log("Error creating socket!");
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    if (bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
    {
        Log("Bind failed!");
        closesocket(ListenSocket);
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }
    freeaddrinfo(result);

    if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        Log("Listen failed!");
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }

    Log("Server is running. Waiting for connections...");

    while (true)
    {
        SOCKET ClientSocket = accept(ListenSocket, NULL, NULL);
        if (ClientSocket != INVALID_SOCKET)
        {
            std::thread(HandleClient, ClientSocket).detach();
        }
    }

    closesocket(ListenSocket);
    WSACleanup();
    return 0;
}