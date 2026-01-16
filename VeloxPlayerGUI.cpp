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
#include <fstream>
#include <iostream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "VeloxCore.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

// --- GLOBALS ---
std::vector<std::string> playlist;
int currentTrackIndex = -1;

// Playback Control
std::atomic<bool> isPlaying(false);
std::atomic<bool> isPaused(false);
std::atomic<bool> stopReq(false);

// Time Tracking
std::atomic<size_t> currentFrame(0); 
std::atomic<size_t> totalFrames(0);
std::atomic<uint32_t> currentSampleRate(0);

// Metadata
std::string metaFileName = "No File Loaded";
std::string metaTechSpecs = "--";
std::string metaBitrate = "--";

// UI Handles
HWND hMain, hList, hBtnPlay, hBtnNext, hBtnPrev, hBtnStop, hBtnAddFile, hBtnAddFolder, hSlider, hTimeLabel;
std::thread playerThread;
bool isDraggingSlider = false; 

#define WM_USER_NEXTTRACK (WM_USER + 1)

// --- HELPERS ---
void Log(std::string msg) { std::cout << "[VeloxPlayer] " << msg << std::endl; }

std::string FormatTime(size_t frames, uint32_t sr) {
    if (sr == 0) return "0:00";
    size_t total_sec = frames / sr;
    size_t m = total_sec / 60;
    size_t s = total_sec % 60;
    char buf[16]; sprintf(buf, "%d:%02d", (int)m, (int)s);
    return std::string(buf);
}

int16_t ConvertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int bits) {
    if (isFloat) {
        velox_sample_t m_val = raw;
        uint32_t sign = 0;
        if (m_val < 0) { sign = 1; m_val = -m_val; }
        uint32_t mant = (uint32_t)(m_val & 0x7FFFFF);
        uint32_t u = (sign << 31) | ((uint32_t)exp << 23) | mant;
        float f; memcpy(&f, &u, 4);
        if (f > 1.0f) f = 1.0f; if (f < -1.0f) f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }
    if (bits == 24) return (int16_t)(raw >> 8);
    if (bits == 32) return (int16_t)(raw >> 16);
    return (int16_t)raw;
}

// --- AUDIO THREAD ---
void AudioThread(std::string path, size_t startFrame) {
    Log("Thread Start. Seek: " + std::to_string(startFrame));
    
    std::ifstream in(path, std::ios::binary | std::ios::ate); 
    if (!in.is_open()) return;
    
    size_t fileSize = in.tellg();
    in.seekg(0, std::ios::beg);

    VeloxHeader vh; in.read((char*)&vh, sizeof(vh));
    
    // Update Globals
    currentSampleRate = vh.sample_rate;
    totalFrames = vh.total_samples / vh.channels;
    
    // Metadata UI Update
    std::stringstream ssSpec;
    ssSpec << vh.bits_per_sample << "bit / " << (vh.sample_rate/1000.0) << "kHz";
    if (vh.format_code == 3) ssSpec << " (Float)";
    metaTechSpecs = ssSpec.str();

    double duration = (double)totalFrames / vh.sample_rate;
    if(duration > 0) {
        int kbps = (int)((fileSize * 8) / (duration * 1000));
        metaBitrate = "VLX " + std::to_string(kbps) + " kbps";
    }
    InvalidateRect(hMain, NULL, FALSE);

    // Skip blobs
    in.seekg(vh.header_blob_size, std::ios::cur);
    in.seekg(vh.footer_blob_size, std::ios::cur);
    std::vector<uint8_t> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    VeloxCodec::StreamingDecoder dec(comp.data(), comp.size(), vh.total_samples);
    
    size_t startSample = startFrame * vh.channels;
    if(startSample > 0) {
        dec.SeekToSample(startSample);
        currentFrame = startFrame; 
    }

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = vh.channels;
    wfx.nSamplesPerSec = vh.sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (vh.channels * 16) / 8;
    wfx.nAvgBytesPerSec = vh.sample_rate * wfx.nBlockAlign;

    HWAVEOUT hWo;
    if (waveOutOpen(&hWo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;

    const int BUFFERS = 4;
    const int CHUNK_FRAMES = 4096;
    const int CHUNK_SAMPLES = CHUNK_FRAMES * vh.channels;

    WAVEHDR hdrs[BUFFERS];
    for(int i=0; i<BUFFERS; i++) {
        memset(&hdrs[i], 0, sizeof(WAVEHDR));
        hdrs[i].lpData = (LPSTR)malloc(CHUNK_SAMPLES * 2);
        hdrs[i].dwBufferLength = CHUNK_SAMPLES * 2;
        waveOutPrepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        hdrs[i].dwFlags |= WHDR_DONE;
    }

    std::vector<int16_t> pcmBuffer;
    pcmBuffer.reserve(CHUNK_SAMPLES);

    bool eof = false;
    isPlaying = true;
    size_t framesPlayed = startFrame;

    while (!stopReq) {
        if (isPaused) {
            waveOutPause(hWo); Sleep(50); continue;
        } else {
            waveOutRestart(hWo);
        }

        bool active = false;
        for (int i = 0; i < BUFFERS; i++) {
            if (hdrs[i].dwFlags & WHDR_DONE) {
                if (!eof) {
                    pcmBuffer.clear();
                    for(int s=0; s<CHUNK_SAMPLES; s++) {
                        velox_sample_t v; uint8_t e;
                        if(dec.DecodeNext(v, e)) {
                            pcmBuffer.push_back(ConvertSample(v, e, dec.IsFloat(), vh.bits_per_sample));
                        } else {
                            eof = true; break;
                        }
                    }

                    if (!pcmBuffer.empty()) {
                        memcpy(hdrs[i].lpData, pcmBuffer.data(), pcmBuffer.size() * 2);
                        hdrs[i].dwBufferLength = pcmBuffer.size() * 2;
                        waveOutWrite(hWo, &hdrs[i], sizeof(WAVEHDR));
                        
                        size_t framesInBatch = pcmBuffer.size() / vh.channels;
                        framesPlayed += framesInBatch;
                        currentFrame = framesPlayed;
                        active = true;
                    }
                }
            } else {
                active = true;
            }
        }

        if (!active && eof) break; 
        Sleep(5);
    }

    // Drain
    while(!stopReq) {
        bool done = true;
        for(int i=0; i<BUFFERS; i++) if(!(hdrs[i].dwFlags & WHDR_DONE)) done=false;
        if(done) break;
        Sleep(50);
    }

    waveOutReset(hWo);
    for(int i=0; i<BUFFERS; i++) {
        waveOutUnprepareHeader(hWo, &hdrs[i], sizeof(WAVEHDR));
        free(hdrs[i].lpData);
    }
    waveOutClose(hWo);

    if (!stopReq && eof) PostMessage(hMain, WM_USER_NEXTTRACK, 0, 0);
    
    Log("Thread Stopped");
}

// --- THREAD MANAGEMENT ---
void StopAudioThread() {
    stopReq = true;
    if (playerThread.joinable()) playerThread.join();
    stopReq = false;
    isPlaying = false;
}

void StartPlay(size_t seekStartFrame = 0) {
    StopAudioThread(); 
    
    if (currentTrackIndex < 0 || currentTrackIndex >= playlist.size()) return;

    isPaused = false;
    SetWindowText(hBtnPlay, "||"); 
    
    std::string path = playlist[currentTrackIndex];
    size_t last = path.find_last_of("/\\");
    metaFileName = (last == std::string::npos) ? path : path.substr(last+1);
    metaTechSpecs = "Loading...";
    InvalidateRect(hMain, NULL, FALSE);

    currentFrame = seekStartFrame; 
    
    playerThread = std::thread(AudioThread, path, seekStartFrame);
}

// --- UI LOGIC ---
void TogglePause() {
    if (!isPlaying) {
        int idx = SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (idx != LB_ERR) { currentTrackIndex = idx; StartPlay(); }
    } else {
        isPaused = !isPaused;
        SetWindowText(hBtnPlay, isPaused ? ">" : "||");
    }
}

void NextTrack() {
    if (playlist.empty()) return;
    currentTrackIndex++;
    if (currentTrackIndex >= playlist.size()) currentTrackIndex = 0;
    SendMessage(hList, LB_SETCURSEL, currentTrackIndex, 0);
    StartPlay(0);
}

void PrevTrack() {
    if (playlist.empty()) return;
    currentTrackIndex--;
    if (currentTrackIndex < 0) currentTrackIndex = playlist.size() - 1;
    SendMessage(hList, LB_SETCURSEL, currentTrackIndex, 0);
    StartPlay(0);
}

void StopPlay() {
    StopAudioThread();
    SetWindowText(hBtnPlay, ">");
    currentFrame = 0;
    metaTechSpecs = "--"; metaBitrate = "--"; metaFileName = "Ready";
    InvalidateRect(hMain, NULL, FALSE);
    SendMessage(hSlider, TBM_SETPOS, TRUE, 0);
    SetWindowText(hTimeLabel, "0:00 / 0:00");
}

// --- DRAWING & WNDPROC ---
void DrawUI(HDC hdc, RECT& rcClient) {
    HBRUSH bgBrush = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(hdc, &rcClient, bgBrush); DeleteObject(bgBrush);

    RECT rcInfo = { 0, 0, rcClient.right, 90 };
    HBRUSH infoBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rcInfo, infoBrush); DeleteObject(infoBrush);
    MoveToEx(hdc, 0, 90, NULL); LineTo(hdc, rcClient.right, 90);

    RECT rcArt = { 10, 10, 80, 80 };
    HBRUSH artBrush = CreateSolidBrush(RGB(50, 50, 60)); 
    FillRect(hdc, &rcArt, artBrush); DeleteObject(artBrush);
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255, 255, 255));
    DrawText(hdc, "VELOX", -1, &rcArt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT rcText = { 90, 10, rcClient.right - 10, 35 };
    HFONT hTitleFont = CreateFont(20, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hTitleFont);
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawText(hdc, metaFileName.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, hOldFont); DeleteObject(hTitleFont);

    RECT rcSpecs = { 90, 40, rcClient.right - 10, 60 };
    HFONT hSpecFont = CreateFont(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Segoe UI");
    SelectObject(hdc, hSpecFont);
    std::string infoStr = metaTechSpecs + "  |  " + metaBitrate;
    SetTextColor(hdc, RGB(0, 120, 215)); 
    DrawText(hdc, infoStr.c_str(), -1, &rcSpecs, DT_LEFT | DT_TOP | DT_SINGLELINE);
    SelectObject(hdc, hOldFont); DeleteObject(hSpecFont);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        hMain = hwnd;
        HFONT font = CreateFont(15,0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,0,0,0,0,"Segoe UI");
        hList = CreateWindow("LISTBOX",0,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_NOTIFY, 0,91,480,200,hwnd,(HMENU)3,0,0);
        
        // Trackbar 0-1000
        hSlider = CreateWindow(TRACKBAR_CLASS, "", WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_ENABLESELRANGE, 10,300,360,30,hwnd,(HMENU)9,0,0);
        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));

        hTimeLabel = CreateWindow("STATIC", "0:00 / 0:00", WS_CHILD|WS_VISIBLE|SS_RIGHT, 380, 305, 90, 20, hwnd, 0, 0, 0);
        
        int btnY = 340;
        hBtnAddFile = CreateWindow("BUTTON","+File",WS_CHILD|WS_VISIBLE, 10,btnY,50,25,hwnd,(HMENU)1,0,0);
        hBtnAddFolder = CreateWindow("BUTTON","+Folder",WS_CHILD|WS_VISIBLE, 70,btnY,60,25,hwnd,(HMENU)2,0,0);
        hBtnPrev = CreateWindow("BUTTON","<<",WS_CHILD|WS_VISIBLE, 160,btnY,40,25,hwnd,(HMENU)6,0,0);
        hBtnPlay = CreateWindow("BUTTON",">",WS_CHILD|WS_VISIBLE, 210,btnY,60,25,hwnd,(HMENU)4,0,0);
        hBtnNext = CreateWindow("BUTTON",">>",WS_CHILD|WS_VISIBLE, 280,btnY,40,25,hwnd,(HMENU)7,0,0);
        hBtnStop = CreateWindow("BUTTON","Stop",WS_CHILD|WS_VISIBLE, 330,btnY,50,25,hwnd,(HMENU)5,0,0);

        EnumChildWindows(hwnd, [](HWND h, LPARAM f){ SendMessage(h,WM_SETFONT,f,1); return TRUE; }, (LPARAM)font);
        SetTimer(hwnd, 1, 200, 0);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc); DrawUI(hdc, rc); EndPaint(hwnd, &ps); break;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc); int w=rc.right, h=rc.bottom;
        MoveWindow(hList, 0, 91, w, h-140, TRUE);
        int bottomY = h - 45;
        MoveWindow(hSlider, 10, bottomY-35, w-110, 30, TRUE);
        MoveWindow(hTimeLabel, w-100, bottomY-30, 90, 20, TRUE);
        int btnY = bottomY; int cx = w/2;
        MoveWindow(hBtnPrev, cx-80, btnY, 40, 25, TRUE);
        MoveWindow(hBtnPlay, cx-30, btnY, 60, 25, TRUE);
        MoveWindow(hBtnNext, cx+40, btnY, 40, 25, TRUE);
        MoveWindow(hBtnStop, cx+90, btnY, 50, 25, TRUE);
        MoveWindow(hBtnAddFile, 10, btnY, 50, 25, TRUE);
        MoveWindow(hBtnAddFolder, 70, btnY, 60, 25, TRUE);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if(id==1) {
            OPENFILENAMEA ofn={0}; char sz[65535]={0};
            ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd; ofn.lpstrFile=sz; ofn.nMaxFile=65535;
            ofn.lpstrFilter="Velox Files\0*.vlx\0"; ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
            if(GetOpenFileNameA(&ofn)) {
                char* p = sz; std::string dir = p; p += dir.length()+1;
                if(*p==0) { playlist.push_back(dir); SendMessageA(hList,LB_ADDSTRING,0,(LPARAM)dir.substr(dir.find_last_of("/\\")+1).c_str()); }
                else while(*p) { playlist.push_back(dir+"\\"+p); SendMessageA(hList,LB_ADDSTRING,0,(LPARAM)p); p+=strlen(p)+1; }
            }
        }
        if(id==3 && HIWORD(wp)==LBN_DBLCLK) { currentTrackIndex=SendMessage(hList,LB_GETCURSEL,0,0); StartPlay(0); }
        if(id==4) TogglePause(); if(id==5) StopPlay(); if(id==6) PrevTrack(); if(id==7) NextTrack();
        break;
    }
    case WM_HSCROLL: {
        if((HWND)lp==hSlider) {
            if (LOWORD(wp) == TB_THUMBTRACK) {
                isDraggingSlider = true;
                size_t pos = SendMessage(hSlider, TBM_GETPOS, 0, 0);
                if (totalFrames > 0) {
                    size_t target = (size_t)((double)pos / 1000.0 * totalFrames);
                    std::string sCurr = FormatTime(target, currentSampleRate);
                    std::string sTotal = FormatTime(totalFrames, currentSampleRate);
                    SetWindowText(hTimeLabel, (sCurr + " / " + sTotal).c_str());
                }
            }
            if (LOWORD(wp) == TB_ENDTRACK) {
                isDraggingSlider = false;
                if (isPlaying && totalFrames > 0) {
                    size_t pos = SendMessage(hSlider, TBM_GETPOS, 0, 0);
                    size_t target = (size_t)((double)pos / 1000.0 * totalFrames);
                    StartPlay(target); 
                }
            }
        }
        break;
    }
    case WM_USER_NEXTTRACK: NextTrack(); break;
    case WM_TIMER:
        if(isPlaying && totalFrames > 0 && !isDraggingSlider) {
            int pos = (int)((double)currentFrame / totalFrames * 1000.0);
            SendMessage(hSlider, TBM_SETPOS, TRUE, pos);
            
            std::string sCurr = FormatTime(currentFrame, currentSampleRate);
            std::string sTotal = FormatTime(totalFrames, currentSampleRate);
            SetWindowText(hTimeLabel, (sCurr + " / " + sTotal).c_str());
        }
        break;
    case WM_DESTROY: StopPlay(); PostQuitMessage(0); break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    InitCommonControls();
    WNDCLASSEX wc={0}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.lpszClassName="V"; wc.hbrBackground=NULL;
    RegisterClassEx(&wc);
    CreateWindow("V","Velox Player",WS_VISIBLE|WS_OVERLAPPEDWINDOW,100,100,500,450,0,0,h,0);
    MSG msg; while(GetMessage(&msg,0,0,0)) DispatchMessage(&msg);
    return 0;
}