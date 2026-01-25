#define _WIN32_WINNT 0x0500
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "VeloxCore.h"
#include "VeloxMetadata.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

void Log(std::string msg) { std::cout << "[VeloxPlayer] " << msg << std::endl; }

// --- ROBUST RING BUFFER (CRASH FIX) ---
template <typename T>
class RingBuffer
{
    std::vector<T> buffer;
    size_t head = 0, tail = 0, capacity;
    std::mutex mtx;
    std::condition_variable cv_read, cv_write;
    bool finished = false;
    bool canceled = false; 

public:
    RingBuffer(size_t size) : capacity(size + 1), buffer(size + 1) {}

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mtx);
        head = tail = 0;
        finished = false;
        canceled = false;
    }

    void Cancel()
    {
        std::lock_guard<std::mutex> lock(mtx);
        canceled = true;
        finished = true;
        cv_read.notify_all();  
        cv_write.notify_all(); 
    }

    bool Push(const std::vector<T> &data)
    {
        size_t n = data.size();
        size_t written = 0;
        while (written < n)
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv_write.wait(lock, [&]
                          { return canceled || ((head + 1) % capacity) != tail; });

            if (canceled)
                return false; 

            while (written < n && ((head + 1) % capacity) != tail)
            {
                buffer[head] = data[written++];
                head = (head + 1) % capacity;
            }
            cv_read.notify_one();
        }
        return true;
    }

    size_t Pull(std::vector<T> &out, size_t count)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_read.wait(lock, [&]
                     { return canceled || head != tail || finished; });

        if (canceled)
            return 0;

        size_t read = 0;
        while (read < count && head != tail)
        {
            out.push_back(buffer[tail]);
            tail = (tail + 1) % capacity;
            read++;
        }
        cv_write.notify_one();
        return read;
    }

    void SetFinished()
    {
        std::lock_guard<std::mutex> lock(mtx);
        finished = true;
        cv_read.notify_all();
    }

    bool IsFinished()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return finished && (head == tail);
    }
};

// --- GLOBALS ---
std::vector<std::string> playlist;
int currentTrackIndex = -1;

std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<bool> isLooping(false);
std::atomic<bool> stopReq(false);
std::atomic<bool> seekReq(false);
std::atomic<size_t> seekTarget(0);

std::atomic<size_t> currentFrame(0);
std::atomic<size_t> totalFrames(0);
std::atomic<uint32_t> currentSampleRate(0);

std::string metaTitle = "Velox Player";
std::string metaArtist = "Ready";
std::string metaInfo = "--";
std::string metaBitrate = "--";

RingBuffer<int16_t> audioBuffer(131072);
HWND hMain, hList, hSlider, hTime, hBtnPlay, hBtnLoop;
std::thread decoderThread, outputThread;

#define WM_USER_UPDATE_UI (WM_USER + 1)
#define WM_USER_NEXT (WM_USER + 2)

// --- CONVERTER ---
std::string FormatTime(size_t frames, uint32_t sampleRate)
{
    if (sampleRate == 0)
        sampleRate = 44100;
    size_t totalSeconds = frames / sampleRate;
    size_t minutes = totalSeconds / 60;
    size_t seconds = totalSeconds % 60;
    char buf[32];
    sprintf(buf, "%zu:%02zu", minutes, seconds);
    return std::string(buf);
}

int16_t ConvertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int floatMode, int bits)
{
    if (isFloat)
    {
        if (floatMode == 0)
        {
            velox_sample_t m_val = raw;
            uint32_t sign = 0;
            if (m_val < 0)
            {
                sign = 1;
                m_val = -m_val;
            }
            uint32_t mant = (uint32_t)(m_val & 0x7FFFFF);
            uint32_t u = (sign << 31) | ((uint32_t)exp << 23) | mant;
            float f;
            memcpy(&f, &u, 4);
            if (std::isnan(f))
                f = 0.0f;
            if (f > 1.0f)
                f = 1.0f;
            if (f < -1.0f)
                f = -1.0f;
            return (int16_t)(f * 32767.0f);
        }
        else if (floatMode == 1)
            return (int16_t)raw;
        else if (floatMode == 2)
            return (int16_t)(raw >> 8);
    }
    if (bits == 24)
        return (int16_t)(raw >> 8);
    if (bits == 32)
        return (int16_t)(raw >> 16);
    return (int16_t)raw;
}

// --- DECODER WORKER ---
void DecoderWorker(std::string path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return;
    size_t fileSize = in.tellg();
    in.seekg(0, std::ios::beg);

    VeloxHeader vh;
    in.read((char *)&vh, sizeof(vh));

    // Metadata UI Update
    std::string t, a;
    if (vh.version >= 0x0400)
    {
        VeloxMetadata meta;
        if (meta.ReadFromStream(in))
        {
            t = meta.GetTag("TITLE");
            a = meta.GetTag("ARTIST");
        }
    }
    size_t p = path.find_last_of("/\\");
    std::string fname = (p == std::string::npos) ? path : path.substr(p + 1);
    metaTitle = t.empty() ? fname : t;
    metaArtist = a.empty() ? "Unknown Artist" : a;

    currentSampleRate = vh.sample_rate;
    totalFrames = vh.total_samples / vh.channels;

    std::stringstream ss;
    ss << vh.bits_per_sample << "bit / " << (vh.sample_rate / 1000.0) << "kHz";
    if (vh.format_code == 3)
        ss << " Float";
    metaInfo = ss.str();

    double duration = (double)totalFrames / vh.sample_rate;
    if (duration > 0)
    {
        int kbps = (int)((fileSize * 8) / (duration * 1000));
        metaBitrate = "VLX " + std::to_string(kbps) + " kbps";
    }
    PostMessage(hMain, WM_USER_UPDATE_UI, 0, 0);

    in.seekg(vh.header_blob_size, std::ios::cur);
    in.seekg(vh.footer_blob_size, std::ios::cur);

    std::vector<uint8_t> compData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    VeloxCodec::StreamingDecoder dec(compData.data(), compData.size(), vh.total_samples);
    int floatMode = dec.GetFloatMode();
    bool isFloat = (vh.format_code == 3);

    std::vector<int16_t> pcmBatch;
    pcmBatch.reserve(4096);
    size_t samplesDecoded = 0;

    while (!stopReq)
    {
        if (seekReq)
        {
            audioBuffer.Reset(); 
            size_t targetSample = seekTarget * vh.channels;
            dec = VeloxCodec::StreamingDecoder(compData.data(), compData.size(), vh.total_samples);
            floatMode = dec.GetFloatMode();

            samplesDecoded = 0;
            velox_sample_t dv;
            uint8_t de;
            while (samplesDecoded < targetSample && !stopReq)
            {
                if (!dec.DecodeNext(dv, de))
                    break;
                samplesDecoded++;
            }
            seekReq = false;
        }

        if (isPaused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        for (int k = 0; k < 4096; k++)
        {
            velox_sample_t val;
            uint8_t exp;
            if (!dec.DecodeNext(val, exp))
            {
                audioBuffer.SetFinished();
                return; // EOF
            }
            pcmBatch.push_back(ConvertSample(val, exp, isFloat, floatMode, vh.bits_per_sample));
            samplesDecoded++;
        }

        if (!audioBuffer.Push(pcmBatch))
            return;

        pcmBatch.clear();
        currentFrame = samplesDecoded / vh.channels;
    }
}

// --- OUTPUT WORKER ---
void OutputWorker(uint32_t sampleRate, uint16_t channels)
{
    if (sampleRate == 0)
        sampleRate = 44100;
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (channels * 16) / 8;
    wfx.nAvgBytesPerSec = sampleRate * wfx.nBlockAlign;

    HWAVEOUT hWo;
    if (waveOutOpen(&hWo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return;

    const int BUFFERS = 4;
    const int SIZE = 8192;
    WAVEHDR hdrs[BUFFERS];

    for (int i = 0; i < BUFFERS; i++)
    {
        memset(&hdrs[i], 0, sizeof(WAVEHDR));
        hdrs[i].lpData = (LPSTR)malloc(SIZE * 2);
        hdrs[i].dwBufferLength = SIZE * 2;
        waveOutPrepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        hdrs[i].dwFlags |= WHDR_DONE;
    }

    std::vector<int16_t> chunk;
    chunk.reserve(SIZE);

    // Pre-fill
    int retry = 0;
    while (!stopReq && retry++ < 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

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

        bool active = false;
        for (int i = 0; i < BUFFERS; i++)
        {
            if (hdrs[i].dwFlags & WHDR_DONE)
            {
                chunk.clear();
                size_t pulled = audioBuffer.Pull(chunk, SIZE);

                if (pulled > 0)
                {
                    memcpy(hdrs[i].lpData, chunk.data(), pulled * 2);
                    hdrs[i].dwBufferLength = pulled * 2;
                    waveOutWrite(hWo, &hdrs[i], sizeof(WAVEHDR));
                    active = true;
                }
                else if (audioBuffer.IsFinished())
                {
                    goto CleanExit;
                }
            }
            else
                active = true;
        }
        if (!active)
            Sleep(5);
    }

CleanExit:
    while (!stopReq)
    {
        bool done = true;
        for (int i = 0; i < BUFFERS; i++)
            if (!(hdrs[i].dwFlags & WHDR_DONE))
                done = false;
        if (done)
            break;
        Sleep(50);
    }

    waveOutReset(hWo);
    for (int i = 0; i < BUFFERS; i++)
    {
        waveOutUnprepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        free(hdrs[i].lpData);
    }
    waveOutClose(hWo);
    if (!stopReq)
        PostMessage(hMain, WM_USER_NEXT, 0, 0);
}

// --- CONTROLLER ---
void StopAll()
{
    stopReq = true;
    audioBuffer.Cancel(); 

    // Join threads 
    if (decoderThread.joinable())
        decoderThread.join();
    if (outputThread.joinable())
        outputThread.join();

    stopReq = false;
    isPlaying = false;
    audioBuffer.Reset(); 
}

void PlayTrack(int index)
{
    if (index < 0 || index >= playlist.size())
        return;
    StopAll(); 

    currentTrackIndex = index;
    currentFrame = 0;

    std::ifstream in(playlist[index], std::ios::binary);
    if (!in.is_open())
        return;
    VeloxHeader vh;
    in.read((char *)&vh, sizeof(vh));
    in.close();

    isPlaying = true;
    isPaused = false;
    SetWindowText(hBtnPlay, "||");
    decoderThread = std::thread(DecoderWorker, playlist[index]);
    outputThread = std::thread(OutputWorker, vh.sample_rate, vh.channels);
}

void NextTrack()
{
    if (playlist.empty())
        return;
    int next = currentTrackIndex + 1;
    if (next >= playlist.size())
    {
        if (isLooping)
            next = 0;
        else
            return;
    }
    SendMessage(hList, LB_SETCURSEL, next, 0);
    PlayTrack(next);
}

void PrevTrack()
{
    if (playlist.empty())
        return;
    int next = currentTrackIndex - 1;
    if (next < 0)
        next = playlist.size() - 1;
    SendMessage(hList, LB_SETCURSEL, next, 0);
    PlayTrack(next);
}

void TogglePause()
{
    if (!isPlaying)
    {
        if (!playlist.empty())
            PlayTrack(currentTrackIndex >= 0 ? currentTrackIndex : 0);
    }
    else
    {
        isPaused = !isPaused;
        SetWindowText(hBtnPlay, isPaused ? ">" : "||");
    }
}

// --- GUI ---
void AddPath(std::string path)
{
    playlist.push_back(path);
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
}

void OpenDlg(bool folder)
{
    if (!folder)
    {
        char *buf = new char[65536];
        buf[0] = 0;
        OPENFILENAMEA ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hMain;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = 65536;
        ofn.lpstrFilter = "Velox Files\0*.vlx\0";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

        if (GetOpenFileNameA(&ofn))
        {
            std::string dir = buf;
            char *p = buf + dir.length() + 1;
            if (*p == 0)
                AddPath(dir);
            else
                while (*p)
                {
                    AddPath(dir + "\\" + p);
                    p += strlen(p) + 1;
                }
        }
        delete[] buf;
    }
    else
    {
        BROWSEINFOA bi = {0};
        bi.hwndOwner = hMain;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl)
        {
            char path[MAX_PATH];
            SHGetPathFromIDListA(pidl, path);
            std::string search = std::string(path) + "\\*.vlx";
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(search.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    AddPath(std::string(path) + "\\" + fd.cFileName);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
    }
}

void DrawUI(HDC hdc, RECT rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 35));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    RECT rcArt = {20, 20, 100, 100};
    HBRUSH artBrush = CreateSolidBrush(RGB(50, 50, 60));
    FillRect(hdc, &rcArt, artBrush);
    DeleteObject(artBrush);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 255, 128));
    HFONT hLogo = CreateFont(40, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, "Consolas");
    SelectObject(hdc, hLogo);
    DrawText(hdc, "V", -1, &rcArt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(hLogo);

    RECT rcInfo = {120, 25, rc.right - 20, 55};
    HFONT hBig = CreateFont(24, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI");
    SelectObject(hdc, hBig);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawText(hdc, metaTitle.c_str(), -1, &rcInfo, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    DeleteObject(hBig);

    RECT rcArtName = {120, 55, rc.right - 20, 75};
    HFONT hSmall = CreateFont(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI");
    SelectObject(hdc, hSmall);
    SetTextColor(hdc, RGB(200, 200, 200));
    DrawText(hdc, metaArtist.c_str(), -1, &rcArtName, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT rcTech = {120, 80, rc.right - 20, 100};
    SetTextColor(hdc, RGB(0, 150, 255));
    std::string info = metaInfo + "  •  " + metaBitrate;
    DrawText(hdc, info.c_str(), -1, &rcTech, DT_LEFT | DT_TOP | DT_SINGLELINE);
    DeleteObject(hSmall);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        hMain = hwnd;
        hList = CreateWindow("LISTBOX", 0, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 20, 120, 440, 230, hwnd, (HMENU)100, 0, 0);
        hSlider = CreateWindow(TRACKBAR_CLASS, 0, WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_ENABLESELRANGE, 20, 360, 440, 30, hwnd, (HMENU)101, 0, 0);
        SendMessage(hSlider, TBM_SETRANGE, 1, MAKELPARAM(0, 1000));
        hTime = CreateWindow("STATIC", "0:00 / 0:00", WS_CHILD | WS_VISIBLE | SS_CENTER, 190, 390, 100, 20, hwnd, 0, 0, 0);

        int y = 420;
        hBtnLoop = CreateWindow("BUTTON", "Loop: Off", WS_CHILD | WS_VISIBLE, 20, y, 80, 30, hwnd, (HMENU)8, 0, 0);
        CreateWindow("BUTTON", "<<", WS_CHILD | WS_VISIBLE, 140, y, 50, 30, hwnd, (HMENU)2, 0, 0);
        hBtnPlay = CreateWindow("BUTTON", ">", WS_CHILD | WS_VISIBLE, 200, y, 80, 30, hwnd, (HMENU)1, 0, 0);
        CreateWindow("BUTTON", ">>", WS_CHILD | WS_VISIBLE, 290, y, 50, 30, hwnd, (HMENU)3, 0, 0);
        CreateWindow("BUTTON", "+", WS_CHILD | WS_VISIBLE, 410, y, 50, 30, hwnd, (HMENU)4, 0, 0);

        SetTimer(hwnd, 1, 200, 0);
        break;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == 1)
            TogglePause();
        if (id == 2)
            PrevTrack();
        if (id == 3)
            NextTrack();
        if (id == 4)
            OpenDlg(false);
        if (id == 8)
        {
            isLooping = !isLooping;
            SetWindowText(hBtnLoop, isLooping ? "Loop: ON" : "Loop: Off");
        }
        if (id == 100 && HIWORD(wp) == LBN_DBLCLK)
        {
            int idx = SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (idx != -1)
                PlayTrack(idx);
        }
        break;
    }
    case WM_HSCROLL:
        if ((HWND)lp == hSlider && LOWORD(wp) == TB_ENDTRACK)
        {
            if (isPlaying && totalFrames > 0)
            {
                size_t pos = SendMessage(hSlider, TBM_GETPOS, 0, 0);
                seekTarget = (size_t)((double)pos / 1000.0 * totalFrames);
                seekReq = true;
            }
        }
        break;
    case WM_USER_UPDATE_UI:
        InvalidateRect(hwnd, 0, 0);
        break;
    case WM_USER_NEXT:
        NextTrack();
        break;
    case WM_TIMER:
        if (isPlaying && totalFrames > 0 && !seekReq)
        {
            int pos = (int)((double)currentFrame / totalFrames * 1000);
            SendMessage(hSlider, TBM_SETPOS, 1, pos);
            char buf[64];
            sprintf(buf, "%s / %s", FormatTime(currentFrame, currentSampleRate).c_str(), FormatTime(totalFrames, currentSampleRate).c_str());
            SetWindowText(hTime, buf);
        }
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        DrawUI(hdc, rc);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_DESTROY:
        StopAll();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int)
{
    InitCommonControls();
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "V";
    wc.hbrBackground = NULL;
    RegisterClassEx(&wc);
    CreateWindow("V", "Velox Player v1.0", WS_VISIBLE | WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 200, 200, 500, 520, 0, 0, h, 0);
    MSG msg;
    while (GetMessage(&msg, 0, 0, 0))
        DispatchMessage(&msg);
    return 0;
}