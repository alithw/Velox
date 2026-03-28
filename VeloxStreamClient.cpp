#define _WIN32_WINNT 0x0501
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <iostream>
#include <cmath>

#include "VeloxCore.h"
#include "VeloxMetadata.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

void Log(std::string msg) { std::cout << "[Client] " << msg << std::endl; }

// --- NETWORK CORE ---
class NetClient
{
    SOCKET ConnectSocket = INVALID_SOCKET;

public:
    bool Connect(std::string ip, std::string port)
    {
        if (ConnectSocket != INVALID_SOCKET)
        {
            closesocket(ConnectSocket);
            ConnectSocket = INVALID_SOCKET;
        }
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        struct addrinfo *result = NULL, hints;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(ip.c_str(), port.c_str(), &hints, &result) != 0)
            return false;
        ConnectSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (connect(ConnectSocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
            return false;
        return true;
    }

    std::vector<uint8_t> Request(std::string cmd)
    {
        if (send(ConnectSocket, cmd.c_str(), cmd.length() + 1, 0) == SOCKET_ERROR)
            return {};

        uint32_t netSize = 0;
        char *sizePtr = (char *)&netSize;
        int sizeBytesRead = 0;
        while (sizeBytesRead < 4)
        {
            int res = recv(ConnectSocket, sizePtr + sizeBytesRead, 4 - sizeBytesRead, 0);
            if (res <= 0)
                return {};
            sizeBytesRead += res;
        }

        uint32_t size = ntohl(netSize);
        if (size == 0)
            return {};

        std::vector<uint8_t> buffer(size);
        size_t totalRead = 0;
        while (totalRead < size)
        {
            int bytes = recv(ConnectSocket, (char *)buffer.data() + totalRead, size - totalRead, 0);
            if (bytes <= 0)
                break;
            totalRead += bytes;
        }
        return buffer;
    }
};

// --- RING BUFFER ---
template <typename T>
class RingBuffer
{
    std::vector<T> buffer;
    size_t head = 0, tail = 0, capacity;
    std::mutex mtx;
    std::condition_variable cv_read, cv_write;
    bool finished = false, canceled = false;

public:
    RingBuffer(size_t size) : capacity(size + 1), buffer(size + 1) {}
    void Reset()
    {
        std::lock_guard<std::mutex> lk(mtx);
        head = tail = 0;
        finished = canceled = false;
    }
    void Cancel()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            canceled = true;
            finished = true;
        }
        cv_read.notify_all();
        cv_write.notify_all();
    }
    bool Push(const std::vector<T> &d)
    {
        size_t n = d.size(), w = 0;
        while (w < n)
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv_write.wait(lk, [&]
                          { return canceled || ((head + 1) % capacity) != tail; });
            if (canceled)
                return false;
            while (w < n && ((head + 1) % capacity) != tail)
            {
                buffer[head] = d[w++];
                head = (head + 1) % capacity;
            }
            cv_read.notify_one();
        }
        return true;
    }
    size_t Pull(std::vector<T> &out, size_t count)
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv_read.wait(lk, [&]
                     { return canceled || head != tail || finished; });
        if (canceled)
            return 0;
        size_t r = 0;
        while (r < count && head != tail)
        {
            out.push_back(buffer[tail]);
            tail = (tail + 1) % capacity;
            r++;
        }
        cv_write.notify_one();
        return r;
    }
    void SetFinished()
    {
        std::lock_guard<std::mutex> lk(mtx);
        finished = true;
        cv_read.notify_all();
    }
    bool IsFinished()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return finished && head == tail;
    }
};

// --- GLOBALS ---
NetClient server;
struct ServerTrack
{
    int id;
    std::string name;
    uint32_t size;
};
std::vector<ServerTrack> playlist;
int currentTrackIdx = -1;

std::atomic<bool> isPlaying(false), isPaused(false), stopReq(false);

// Seek variables
std::atomic<bool> seekReq(false);
std::atomic<size_t> seekTargetSample(0);

std::atomic<size_t> currentFrame(0), totalFrames(0);
std::atomic<uint32_t> currentSampleRate(0), currentChannels(0);

// STREAMING GLOBALS
std::vector<uint8_t> *globalFileRAM = nullptr;
std::atomic<size_t> downloadedBytes(0);
std::atomic<size_t> targetFileSize(0);

// BUFFER LIMIT
const size_t MAX_BUFFER_AHEAD = 7 * 1024 * 1024;
std::atomic<size_t> currentDecoderBytePos(0);

RingBuffer<int16_t> audioBuffer(65536); // Small buffer
HWND hMain, hList, hSlider, hTime, hBtnPlay;
std::thread downloadThread, decoderThread, outputThread;

#define WM_UPDATE_UI (WM_USER + 1)
#define WM_NEXT_TRACK (WM_USER + 2)

std::string uiTitle = "Velox Streamer";
std::string uiStatus = "Connecting...";
std::string uiBufferInfo = "";

// --- CONVERTER ---
int16_t ConvertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int floatMode, int bits)
{
    if (isFloat)
    {
        if (floatMode == 0)
        {
            velox_sample_t m = raw;
            uint32_t s = 0;
            if (m < 0)
            {
                s = 1;
                m = -m;
            }
            uint32_t ma = (uint32_t)(m & 0x7FFFFF);
            uint32_t u = (s << 31) | ((uint32_t)exp << 23) | ma;
            float f;
            memcpy(&f, &u, 4);
            if (std::isnan(f))
                f = 0;
            if (f > 1.0f)
                f = 1.0f;
            if (f < -1.0f)
                f = -1.0f;
            return (int16_t)(f * 32767.0f);
        }
        else
            return (floatMode == 1) ? (int16_t)raw : (int16_t)(raw >> 8);
    }
    if (bits == 24)
        return (int16_t)(raw >> 8);
    if (bits == 32)
        return (int16_t)(raw >> 16);
    return (int16_t)raw;
}

// --- THREAD 3: AUDIO OUTPUT ---
void OutputWorker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!stopReq && currentSampleRate == 0)
        Sleep(10);
    if (stopReq)
        return;

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = currentChannels;
    wfx.nSamplesPerSec = currentSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (currentChannels * 16) / 8;
    wfx.nAvgBytesPerSec = currentSampleRate * wfx.nBlockAlign;

    HWAVEOUT hWo;
    waveOutOpen(&hWo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    const int N = 4, S = 16384;
    WAVEHDR hdrs[N];
    for (int i = 0; i < N; i++)
    {
        memset(&hdrs[i], 0, sizeof(WAVEHDR));
        hdrs[i].lpData = (LPSTR)malloc(S * 2);
        hdrs[i].dwBufferLength = S * 2;
        waveOutPrepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        hdrs[i].dwFlags |= WHDR_DONE;
    }

    std::vector<int16_t> chunk;
    chunk.reserve(S);

    while (!stopReq)
    {
        if (isPaused)
        {
            waveOutPause(hWo);
            Sleep(50);
            continue;
        }
        else
            waveOutRestart(hWo);
        bool act = false;
        for (int i = 0; i < N; i++)
        {
            if (hdrs[i].dwFlags & WHDR_DONE)
            {
                chunk.clear();
                size_t p = audioBuffer.Pull(chunk, S);
                if (p > 0)
                {
                    memcpy(hdrs[i].lpData, chunk.data(), p * 2);
                    hdrs[i].dwBufferLength = p * 2;
                    waveOutWrite(hWo, &hdrs[i], sizeof(WAVEHDR));
                    act = true;
                }
                else if (audioBuffer.IsFinished())
                    goto Exit;
            }
            else
                act = true;
        }
        if (!act)
            Sleep(5);
    }
Exit:
    while (!stopReq)
    {
        bool d = true;
        for (int i = 0; i < N; i++)
            if (!(hdrs[i].dwFlags & WHDR_DONE))
                d = false;
        if (d)
            break;
        Sleep(50);
    }
    waveOutReset(hWo);
    for (int i = 0; i < N; i++)
    {
        waveOutUnprepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        free(hdrs[i].lpData);
    }
    waveOutClose(hWo);
    if (!stopReq)
        PostMessage(hMain, WM_NEXT_TRACK, 0, 0);
}

// --- THREAD 2: DECODER (Producer & Seeker) ---
void DecoderWorker(size_t trackSize)
{
    Log("Decoder waiting for header data...");
    // Wait for header data to load
    while (downloadedBytes < 65536 && downloadedBytes < trackSize && !stopReq)
    {
        Sleep(10);
    }
    if (stopReq)
        return;

    struct MemStream
    {
        const uint8_t *ptr;
        size_t size;
        size_t pos = 0;
        void read(char *dst, size_t sz)
        {
            memcpy(dst, ptr + pos, sz);
            pos += sz;
        }
        void seekg(size_t off) { pos += off; }
    };
    MemStream ms;
    ms.ptr = globalFileRAM->data();
    ms.size = trackSize;

    VeloxHeader vh;
    ms.read((char *)&vh, sizeof(vh));
    currentSampleRate = vh.sample_rate;
    currentChannels = vh.channels;
    totalFrames = vh.total_samples / vh.channels;

    outputThread = std::thread(OutputWorker);

    if (vh.version >= 0x0400)
    {
        uint32_t mSize;
        ms.read((char *)&mSize, 4);
        ms.seekg(mSize);
    }
    ms.seekg(vh.header_blob_size);
    ms.seekg(vh.footer_blob_size);

    size_t dataStartOffset = ms.pos;
    size_t compSize = trackSize - dataStartOffset;

    VeloxCodec::StreamingDecoder dec(ms.ptr + ms.pos, compSize, vh.total_samples);
    int floatMode = dec.GetFloatMode();
    bool isFloat = (vh.format_code == 3);

    size_t localDecoded = 0;
    std::vector<int16_t> pcmBatch;
    pcmBatch.reserve(4096);

    while (!stopReq && localDecoded < vh.total_samples)
    {

        // --- HANDLE SEEK ---
        if (seekReq)
        {
            audioBuffer.Reset();
            size_t targetSample = seekTargetSample;

            dec = VeloxCodec::StreamingDecoder(ms.ptr + dataStartOffset, compSize, vh.total_samples);
            floatMode = dec.GetFloatMode();
            localDecoded = 0;

            Log("Seeking... Fast-forwarding in RAM...");
            uiStatus = "Seeking...";
            PostMessage(hMain, WM_UPDATE_UI, 0, 0);

            // Fast-forward
            velox_sample_t val;
            uint8_t exp;
            while (localDecoded < targetSample && !stopReq)
            {
                // Ensure network has loaded the segment for seeking
                size_t approxPos = dataStartOffset + ((localDecoded * compSize) / vh.total_samples);
                while (downloadedBytes < approxPos + 65536 && downloadedBytes < trackSize && !stopReq)
                {
                    uiBufferInfo = "(Wait Net...)";
                    PostMessage(hMain, WM_UPDATE_UI, 0, 0);
                    Sleep(20);
                }
                uiBufferInfo = "";

                if (!dec.DecodeNext(val, exp))
                    break;
                localDecoded++;
            }
            seekReq = false;
            uiStatus = "Playing";
            PostMessage(hMain, WM_UPDATE_UI, 0, 0);
        }

        // Notify Downloader of Decoder position for automatic Sleep/Wakeup
        currentDecoderBytePos = dataStartOffset + ((localDecoded * compSize) / vh.total_samples);

        // Wait for network data (If network is slow)
        while (downloadedBytes < currentDecoderBytePos + 16384 && downloadedBytes < trackSize && !stopReq)
        {
            uiBufferInfo = "(Buffering...)";
            PostMessage(hMain, WM_UPDATE_UI, 0, 0);
            Sleep(20);
        }
        uiBufferInfo = "";

        if (stopReq)
            break;
        if (isPaused)
        {
            Sleep(50);
            continue;
        }

        // Decode Chunk
        int batch = 0;
        while (batch < 4096 && localDecoded < vh.total_samples && !stopReq)
        {
            velox_sample_t val;
            uint8_t exp;
            if (!dec.DecodeNext(val, exp))
                break;
            pcmBatch.push_back(ConvertSample(val, exp, isFloat, floatMode, vh.bits_per_sample));
            localDecoded++;
            batch++;
        }

        if (!audioBuffer.Push(pcmBatch))
            break; // If canceled then break
        pcmBatch.clear();

        currentFrame = localDecoded / vh.channels;
    }
    audioBuffer.SetFinished();
    Log("Decoder finished.");
}

// --- THREAD 1: DOWNLOADER (YOUTUBE BUFFERING) ---
void DownloadWorker(int trackIndex)
{
    auto track = playlist[trackIndex];
    uiTitle = track.name;
    uiStatus = "Connecting...";
    PostMessage(hMain, WM_UPDATE_UI, 0, 0);

    // Allocate once for file, max RAM = FileSize (usually 20-30MB)
    globalFileRAM = new std::vector<uint8_t>();
    globalFileRAM->resize(track.size);
    targetFileSize = track.size;
    downloadedBytes = 0;

    decoderThread = std::thread(DecoderWorker, track.size);

    size_t downloaded = 0;
    while (downloaded < track.size && !stopReq)
    {

        // --- YOUTUBE SMART BUFFER LIMIT ---
        // If downloaded exceeds playback point +5MB -> Stop downloading to save bandwidth!
        if (downloaded > currentDecoderBytePos + MAX_BUFFER_AHEAD && !seekReq)
        {
            Sleep(100);
            continue;
        }

        size_t chunk = std::min((size_t)262144, (size_t)track.size - downloaded); // 256KB/lần
        std::vector<uint8_t> data = server.Request("GET " + std::to_string(track.id) + " " + std::to_string(downloaded) + " " + std::to_string(chunk));

        if (data.empty())
        {
            Log("Network Error");
            break;
        }

        memcpy(globalFileRAM->data() + downloaded, data.data(), data.size());
        downloaded += data.size();
        downloadedBytes = downloaded;

        PostMessage(hMain, WM_UPDATE_UI, 0, 0);
    }
}

// --- CONTROLLER ---
void StopAll()
{
    stopReq = true;
    audioBuffer.Cancel();
    if (downloadThread.joinable())
        downloadThread.join();
    if (decoderThread.joinable())
        decoderThread.join();
    if (outputThread.joinable())
        outputThread.join();
    stopReq = false;
    isPlaying = false;
    audioBuffer.Reset();
    if (globalFileRAM)
    {
        delete globalFileRAM;
        globalFileRAM = nullptr;
    }
}

void PlayTrack(int idx)
{
    if (idx < 0 || idx >= playlist.size())
        return;
    StopAll();
    currentTrackIdx = idx;
    isPlaying = true;
    isPaused = false;
    SetWindowText(hBtnPlay, "||");
    downloadThread = std::thread(DownloadWorker, idx);
}
void ToggleP()
{
    if (!isPlaying)
    {
        int i = SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (i != -1)
            PlayTrack(i);
    }
    else
    {
        isPaused = !isPaused;
        SetWindowText(hBtnPlay, isPaused ? ">" : "||");
    }
}
void NextT()
{
    if (playlist.empty())
        return;
    int n = currentTrackIdx + 1;
    if (n >= playlist.size())
        n = 0;
    SendMessage(hList, LB_SETCURSEL, n, 0);
    PlayTrack(n);
}
void PrevT()
{
    if (playlist.empty())
        return;
    int n = currentTrackIdx - 1;
    if (n < 0)
        n = playlist.size() - 1;
    SendMessage(hList, LB_SETCURSEL, n, 0);
    PlayTrack(n);
}

// --- GUI WNDPROC ---
void DrawUI(HDC hdc, RECT rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 25));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    // Title
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOut(hdc, 20, 15, uiTitle.c_str(), uiTitle.length());

    // Status
    SetTextColor(hdc, RGB(0, 150, 255));
    std::string stat = uiStatus + " " + uiBufferInfo;
    TextOut(hdc, 20, 35, stat.c_str(), stat.length());

    if (targetFileSize > 0)
    {
        int barX = 20, barY = 65, barW = rc.right - 40, barH = 6;

        RECT rcBg = {barX, barY, barX + barW, barY + barH};
        HBRUSH hBg = CreateSolidBrush(RGB(50, 50, 60));
        FillRect(hdc, &rcBg, hBg);
        DeleteObject(hBg);

        int dlW = (int)((double)downloadedBytes / targetFileSize * barW);
        RECT rcDl = {barX, barY, barX + dlW, barY + barH};
        HBRUSH hDl = CreateSolidBrush(RGB(120, 120, 130));
        FillRect(hdc, &rcDl, hDl);
        DeleteObject(hDl);

        if (totalFrames > 0)
        {
            int pW = (int)((double)currentFrame / totalFrames * barW);
            RECT rcPl = {barX, barY, barX + pW, barY + barH};
            HBRUSH hPl = CreateSolidBrush(RGB(0, 200, 100));
            FillRect(hdc, &rcPl, hPl);
            DeleteObject(hPl);
        }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CREATE)
    {
        hMain = h;
        HFONT f = CreateFont(14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI");
        hList = CreateWindow("LISTBOX", 0, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 90, 360, 120, h, (HMENU)100, 0, 0);

        hSlider = CreateWindow(TRACKBAR_CLASS, 0, WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_ENABLESELRANGE, 15, 60, 370, 15, h, (HMENU)101, 0, 0);
        SendMessage(hSlider, TBM_SETRANGE, 1, MAKELPARAM(0, 1000));

        hTime = CreateWindow("STATIC", "0:00 / 0:00", WS_CHILD | WS_VISIBLE | SS_RIGHT, 280, 35, 90, 20, h, 0, 0, 0);
        hBtnPlay = CreateWindow("BUTTON", "PLAY", WS_CHILD | WS_VISIBLE, 150, 220, 80, 30, h, (HMENU)1, 0, 0);
        CreateWindow("BUTTON", "<<", WS_CHILD | WS_VISIBLE, 90, 220, 50, 30, h, (HMENU)2, 0, 0);
        CreateWindow("BUTTON", ">>", WS_CHILD | WS_VISIBLE, 240, 220, 50, 30, h, (HMENU)3, 0, 0);
        EnumChildWindows(h, [](HWND c, LPARAM f)
                         {SendMessage(c,WM_SETFONT,f,1);return TRUE; }, (LPARAM)f);

        if (server.Connect("127.0.0.1", "6781"))
        {
            auto list = server.Request("LIST");
            std::string res((char *)list.data(), list.size());
            std::istringstream f(res);
            std::string line;
            while (std::getline(f, line))
            {
                if (line.empty())
                    continue;
                ServerTrack tr;
                sscanf(line.c_str(), "%d|", &tr.id);
                size_t p1 = line.find("|");
                size_t p2 = line.find("|", p1 + 1);
                tr.name = line.substr(p1 + 1, p2 - p1 - 1);
                tr.size = std::stoul(line.substr(p2 + 1));
                playlist.push_back(tr);
                SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)tr.name.c_str());
            }
            uiStatus = "Connected!";
        }
        else
            uiStatus = "Server Offline";

        SetTimer(h, 1, 100, 0);
    }
    if (m == WM_COMMAND)
    {
        int id = LOWORD(w);
        if (id == 1)
            ToggleP();
        if (id == 2)
            PrevT();
        if (id == 3)
            NextT();
        if (id == 100 && HIWORD(w) == LBN_DBLCLK)
            PlayTrack(SendMessage(hList, LB_GETCURSEL, 0, 0));
    }
    if (m == WM_HSCROLL && (HWND)l == hSlider && LOWORD(w) == TB_ENDTRACK && isPlaying && totalFrames > 0)
    {
        size_t pos = SendMessage(hSlider, TBM_GETPOS, 0, 0);
        seekTargetSample = (size_t)((double)pos / 1000.0 * totalFrames * currentChannels);
        seekReq = true;
    }
    if (m == WM_UPDATE_UI)
        InvalidateRect(h, 0, 0);
    if (m == WM_NEXT_TRACK)
        NextT();
    if (m == WM_TIMER && isPlaying && totalFrames > 0 && !seekReq)
    {
        int pos = (int)((double)currentFrame / totalFrames * 1000.0);
        SendMessage(hSlider, TBM_SETPOS, 1, pos);
        size_t sf = currentFrame / currentSampleRate, tf = totalFrames / currentSampleRate;
        char buf[64];
        sprintf(buf, "%d:%02d / %d:%02d", (int)sf / 60, (int)sf % 60, (int)tf / 60, (int)tf % 60);
        SetWindowText(hTime, buf);
        InvalidateRect(h, 0, 0); 
    }
    if (m == WM_PAINT)
    {
        PAINTSTRUCT p;
        HDC dc = BeginPaint(h, &p);
        RECT r;
        GetClientRect(h, &r);
        DrawUI(dc, r);
        EndPaint(h, &p);
    }
    if (m == WM_DESTROY)
    {
        StopAll();
        PostQuitMessage(0);
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int)
{
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "VSCL";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    RegisterClassEx(&wc);
    CreateWindow("VSCL", "Velox Stream Client", WS_VISIBLE | WS_OVERLAPPEDWINDOW, 200, 200, 400, 300, 0, 0, h, 0);
    MSG m;
    while (GetMessage(&m, 0, 0, 0))
        DispatchMessage(&m);
    return 0;
}