#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <gdiplus.h>
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
#include <codecvt>

#include "VeloxCore.h"
#include "VeloxMetadata.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// UTF-8 conversion utilities
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

void Log(std::string msg) { std::cout << "[VeloxPlayer] " << msg << std::endl; }

// Dynamic ring buffer for audio samples
template<typename T> class RingBuffer {
    std::vector<T> buffer; size_t head=0, tail=0, capacity;
    std::mutex mtx; std::condition_variable cv_read, cv_write;
    bool finished=false, canceled=false;
public:
    // Default initialization (small), will resize later
    RingBuffer() : capacity(1), buffer(1) {} 

    void Resize(size_t size) {
        std::lock_guard<std::mutex> lk(mtx);
        capacity = size + 1;
        buffer.assign(capacity, 0);
        head = tail = 0;
        finished = canceled = false;
        Log("Buffer resized to: " + std::to_string(size) + " samples");
    }

    void Reset() { std::lock_guard<std::mutex> lk(mtx); head=tail=0; finished=canceled=false; }
    void Cancel() { { std::lock_guard<std::mutex> lk(mtx); canceled=true; finished=true; } cv_read.notify_all(); cv_write.notify_all(); }
    
    bool Push(const std::vector<T>& d) {
        size_t n=d.size(), w=0;
        while(w<n) {
            std::unique_lock<std::mutex> lk(mtx);
            cv_write.wait(lk, [&]{return canceled || ((head+1)%capacity)!=tail;});
            if(canceled) return false;
            while(w<n && ((head+1)%capacity)!=tail) { buffer[head]=d[w++]; head=(head+1)%capacity; }
            cv_read.notify_one();
        } return true;
    }
    
    size_t Pull(std::vector<T>& out, size_t count) {
        std::unique_lock<std::mutex> lk(mtx);
        // Timeout 50ms to avoid deadlock, but long enough for audio
        cv_read.wait(lk, [&]{return canceled || head!=tail || finished;});
        if(canceled) return 0;
        size_t r=0;
        while(r<count && head!=tail) { out.push_back(buffer[tail]); tail=(tail+1)%capacity; r++; }
        cv_write.notify_one(); return r;
    }
    
    void SetFinished() { std::lock_guard<std::mutex> lk(mtx); finished=true; cv_read.notify_all(); }
    bool IsFinished() { std::lock_guard<std::mutex> lk(mtx); return finished && head==tail; }
};

// Global playback state
std::vector<std::string> playlist;
int currentTrackIndex = -1;
std::atomic<bool> isPlaying(false), isPaused(false), isLooping(false), stopReq(false), seekReq(false);
std::atomic<size_t> seekTarget(0), currentFrame(0), totalFrames(0);
std::atomic<uint32_t> currentSampleRate(0);

std::wstring uiTitle = L"Velox Player";
std::wstring uiArtist = L"Drag & Drop File";
std::wstring uiInfo = L"--";
std::wstring uiBitrate = L"--";
Image* coverArtImg = nullptr;

// Global Buffer Object
RingBuffer<int16_t> audioBuffer; 

HWND hMain, hList, hSlider, hTime, hBtnPlay, hBtnLoop;
std::thread decoderThread, outputThread;

// Custom window messages
#define WM_USER_UPDATE_UI (WM_USER + 1)
#define WM_USER_NEXT (WM_USER + 2)

// Sample conversion utility
int16_t ConvertSample(velox_sample_t raw, uint8_t exp, bool isFloat, int floatMode, int bits) {
    if(isFloat) {
        if(floatMode==0) {
            velox_sample_t m=raw; uint32_t s=0; if(m<0){s=1;m=-m;}
            uint32_t ma=(uint32_t)(m&0x7FFFFF); uint32_t u=(s<<31)|((uint32_t)exp<<23)|ma;
            float f; memcpy(&f,&u,4);
            if(std::isnan(f)) f=0; if(f>1.0f) f=1.0f; if(f<-1.0f) f=-1.0f;
            return (int16_t)(f*32767.0f);
        } else return (floatMode==1) ? (int16_t)raw : (int16_t)(raw>>8);
    }
    if(bits==24) return (int16_t)(raw>>8); if(bits==32) return (int16_t)(raw>>16);
    return (int16_t)raw;
}

// Decoder thread worker
void DecoderWorker(std::string path) {
    // Increase decoder thread priority to avoid blocking
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    std::wstring wpath = Utf8ToWide(path);
    std::ifstream in(wpath.c_str(), std::ios::binary | std::ios::ate);
    if (!in.is_open()) return;
    size_t fileSize = in.tellg();
    in.seekg(0, std::ios::beg);

    VeloxHeader vh; in.read((char*)&vh, sizeof(vh));
    
    // High-resolution buffer optimization
    // Calculate buffer size based on sample rate
    // Target: Buffer holds at least 2 seconds of audio
    // 768kHz * 2ch = 1.5M samples/sec -> Buffer ~ 3M samples
    size_t requiredBuffer = vh.sample_rate * vh.channels * 2; 
    if (requiredBuffer < 131072) requiredBuffer = 131072; // Min 128KB
    
    // Resize Ring Buffer
    audioBuffer.Resize(requiredBuffer);

    // UI Updates
    delete coverArtImg; coverArtImg = nullptr;
    uiTitle = Utf8ToWide(path.substr(path.find_last_of("/\\")+1)); 
    uiArtist = L"Unknown Artist";

    if (vh.version >= 0x0400) {
        VeloxMetadata meta;
        if (meta.ReadFromStream(in)) {
            std::string t = meta.GetTag("TITLE");
            std::string a = meta.GetTag("ARTIST");
            if(!t.empty()) uiTitle = Utf8ToWide(t);
            if(!a.empty()) uiArtist = Utf8ToWide(a);
            
            if(meta.hasCoverArt) {
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, meta.coverArt.data.size());
                void* pMem = GlobalLock(hMem);
                memcpy(pMem, meta.coverArt.data.data(), meta.coverArt.data.size());
                GlobalUnlock(hMem);
                IStream* pStream = nullptr;
                CreateStreamOnHGlobal(hMem, TRUE, &pStream);
                coverArtImg = Image::FromStream(pStream);
                pStream->Release();
            }
        }
    }

    currentSampleRate = vh.sample_rate;
    totalFrames = vh.total_samples / vh.channels;
    
    std::wstringstream wss; wss << vh.bits_per_sample << L"bit / " << (vh.sample_rate/1000.0) << L"kHz";
    if(vh.format_code==3) wss << L" Float";
    uiInfo = wss.str();

    double dur = (double)totalFrames / vh.sample_rate;
    if(dur>0) {
        int kbps = (int)((fileSize*8)/(dur*1000));
        uiBitrate = L"VLX " + std::to_wstring(kbps) + L" kbps";
    }
    PostMessage(hMain, WM_USER_UPDATE_UI, 0, 0);

    in.seekg(vh.header_blob_size, std::ios::cur);
    in.seekg(vh.footer_blob_size, std::ios::cur);
    
    std::vector<uint8_t> compData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    VeloxCodec::StreamingDecoder dec(compData.data(), compData.size(), vh.total_samples);
    int floatMode = dec.GetFloatMode();
    bool isFloat = (vh.format_code == 3);
    
    // Increase batch decode size for Hi-Res
    // 4096 is too small for 768kHz. Use 16384 (about 20ms)
    const int BATCH_SIZE = 16384; 
    std::vector<int16_t> pcmBatch; pcmBatch.reserve(BATCH_SIZE);
    size_t samplesDecoded = 0;

    // Load Seek Table logic here (giống v8.1)
    
    while (!stopReq) {
        if (seekReq) {
            audioBuffer.Reset();
            size_t targetSample = seekTarget * vh.channels;
            dec = VeloxCodec::StreamingDecoder(compData.data(), compData.size(), vh.total_samples);
            floatMode = dec.GetFloatMode();
            
            samplesDecoded = 0;
            velox_sample_t dv; uint8_t de;
            // With Hi-Res, quick seek by calling DecodeNext continuously may be slow
            // But currently this is the safest approach
            while(samplesDecoded < targetSample && !stopReq) {
                if(!dec.DecodeNext(dv, de)) break;
                samplesDecoded++;
            }
            seekReq = false;
        }

        if (isPaused) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }

        for(int k=0; k<BATCH_SIZE; k++) {
            velox_sample_t val; uint8_t exp;
            if (!dec.DecodeNext(val, exp)) { audioBuffer.SetFinished(); return; }
            pcmBatch.push_back(ConvertSample(val, exp, isFloat, floatMode, vh.bits_per_sample));
            samplesDecoded++;
        }

        if (!audioBuffer.Push(pcmBatch)) return;
        pcmBatch.clear();
        currentFrame = samplesDecoded / vh.channels;
    }
}

// Output thread worker
void OutputWorker(uint32_t sampleRate, uint16_t channels) {
    // Increase audio thread priority to Time Critical
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    if(sampleRate==0) sampleRate=44100;
    WAVEFORMATEX wfx={0}; wfx.wFormatTag=WAVE_FORMAT_PCM; wfx.nChannels=channels; wfx.nSamplesPerSec=sampleRate; wfx.wBitsPerSample=16; wfx.nBlockAlign=(channels*16)/8; wfx.nAvgBytesPerSec=sampleRate*wfx.nBlockAlign;
    HWAVEOUT hWo; if(waveOutOpen(&hWo,WAVE_MAPPER,&wfx,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR) return;

    // Calculate WaveOut buffer size
    // With 768kHz, we need a larger buffer to avoid underrun
    // Target latency: ~100ms
    int bufferSamples = sampleRate * channels / 10; // 100ms
    if (bufferSamples < 8192) bufferSamples = 8192; // Min size
    if (bufferSamples > 65536) bufferSamples = 65536; // Max clamp

    const int N=4; 
    WAVEHDR hdrs[N];
    for(int i=0;i<N;i++) { 
        memset(&hdrs[i],0,sizeof(WAVEHDR)); 
        hdrs[i].lpData=(LPSTR)malloc(bufferSamples * 2); 
        hdrs[i].dwBufferLength=bufferSamples * 2; 
        waveOutPrepareHeader(hWo,&hdrs[i],sizeof(WAVEHDR)); 
        hdrs[i].dwFlags|=WHDR_DONE; 
    }
    
    std::vector<int16_t> chunk; chunk.reserve(bufferSamples);

    // Wait for pre-fill (IMPORTANT: Wait for buffer at least 50% full before playback)
    // With 4MB RingBuffer, wait about 100ms
    int wait = 0;
    while(!stopReq && wait++ < 50) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    while(!stopReq) {
        if(isPaused) { waveOutPause(hWo); Sleep(50); continue; } else waveOutRestart(hWo);
        bool act=false;
        for(int i=0;i<N;i++) {
            if(hdrs[i].dwFlags&WHDR_DONE) {
                chunk.clear(); 
                size_t p=audioBuffer.Pull(chunk, bufferSamples);
                if(p>0) { 
                    memcpy(hdrs[i].lpData,chunk.data(),p*2); 
                    hdrs[i].dwBufferLength=p*2; 
                    waveOutWrite(hWo,&hdrs[i],sizeof(WAVEHDR)); 
                    act=true; 
                }
                else if(audioBuffer.IsFinished()) goto Exit;
            } else act=true;
        } if(!act) Sleep(2); // Very short sleep for faster polling
    }
Exit:
    while(!stopReq) { bool d=true; for(int i=0;i<N;i++) if(!(hdrs[i].dwFlags&WHDR_DONE)) d=false; if(d) break; Sleep(50); }
    waveOutReset(hWo); for(int i=0;i<N;i++) { waveOutUnprepareHeader(hWo,&hdrs[i],sizeof(WAVEHDR)); free(hdrs[i].lpData); } waveOutClose(hWo);
    if(!stopReq) PostMessage(hMain,WM_USER_NEXT,0,0);
}

void StopAll() {
    stopReq=true; audioBuffer.Cancel();
    if(decoderThread.joinable()) decoderThread.join();
    if(outputThread.joinable()) outputThread.join();
    stopReq=false; isPlaying=false; audioBuffer.Reset();
}
void PlayTrack(int idx) {
    if(idx<0||idx>=playlist.size()) return;
    StopAll(); currentTrackIndex=idx; currentFrame=0;
    std::wstring wpath = Utf8ToWide(playlist[idx]);
    std::ifstream in(wpath.c_str(),std::ios::binary); if(!in.is_open()) return;
    VeloxHeader vh; in.read((char*)&vh,sizeof(vh)); in.close();
    isPlaying=true; isPaused=false; SetWindowTextW(hBtnPlay, L"⏸");
    decoderThread=std::thread(DecoderWorker, playlist[idx]);
    outputThread=std::thread(OutputWorker,vh.sample_rate,vh.channels);
}
void NextT() { if(playlist.empty()) return; int n=currentTrackIndex+1; if(n>=playlist.size()) {if(isLooping) n=0; else return;} SendMessage(hList,LB_SETCURSEL,n,0); PlayTrack(n); }
void PrevT() { if(playlist.empty()) return; int n=currentTrackIndex-1; if(n<0) n=playlist.size()-1; SendMessage(hList,LB_SETCURSEL,n,0); PlayTrack(n); }
void ToggleP() { if(!isPlaying) { int i=SendMessage(hList,LB_GETCURSEL,0,0); if(i!=-1) PlayTrack(i); } else { isPaused=!isPaused; SetWindowTextW(hBtnPlay, isPaused?L"▶":L"⏸"); } }

void AddPath(std::wstring path) {
    std::string u8 = WideToUtf8(path); playlist.push_back(u8);
    size_t pos = path.find_last_of(L"/\\");
    std::wstring name = (pos==std::wstring::npos)?path:path.substr(pos+1);
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
}
void OpenD(bool dir) {
    if(!dir) {
        wchar_t buf[65536]={0}; OPENFILENAMEW ofn={0}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hMain; ofn.lpstrFile=buf; ofn.nMaxFile=65536; ofn.lpstrFilter=L"Velox Files\0*.vlx\0"; ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER;
        if(GetOpenFileNameW(&ofn)) {
            std::wstring d=buf; wchar_t* p=buf+d.length()+1;
            if(*p==0) AddPath(d); else while(*p) { AddPath(d+L"\\"+p); p+=wcslen(p)+1; }
        }
    } else {
        BROWSEINFOW bi={0}; bi.hwndOwner=hMain; LPITEMIDLIST p=SHBrowseForFolderW(&bi);
        if(p) { wchar_t pt[MAX_PATH]; SHGetPathFromIDListW(p,pt); std::wstring s=std::wstring(pt)+L"\\*.vlx"; WIN32_FIND_DATAW f; HANDLE h=FindFirstFileW(s.c_str(),&f); if(h!=INVALID_HANDLE_VALUE) { do { AddPath(std::wstring(pt)+L"\\"+f.cFileName); } while(FindNextFileW(h,&f)); FindClose(h); } }
    }
}

void DrawUI(HDC hdc, RECT rc) {
    HBRUSH bg=CreateSolidBrush(RGB(30,30,35)); FillRect(hdc,&rc,bg); DeleteObject(bg);
    Graphics g(hdc); 
    if(coverArtImg) g.DrawImage(coverArtImg, 20, 20, 100, 100);
    else {
        SolidBrush b(Color(50,50,60)); g.FillRectangle(&b, 20, 20, 100, 100);
        Font f(L"Consolas", 40, FontStyleBold); SolidBrush fb(Color(0,255,128));
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"V", -1, &f, RectF(20,20,100,100), &sf, &fb);
    }
    Font ft(L"Segoe UI", 18, FontStyleBold); SolidBrush wt(Color(255,255,255));
    g.DrawString(uiTitle.c_str(), -1, &ft, PointF(130, 20), &wt);
    Font fa(L"Segoe UI", 12); SolidBrush gr(Color(180,180,180));
    g.DrawString(uiArtist.c_str(), -1, &fa, PointF(130, 60), &gr);
    SolidBrush bl(Color(0,150,255));
    g.DrawString((uiInfo + L" • " + uiBitrate).c_str(), -1, &fa, PointF(130, 85), &bl);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if(m==WM_CREATE) {
        hMain=h; HFONT f=CreateFontW(16,0,0,0,FW_NORMAL,0,0,0,0,0,0,0,0,L"Segoe UI");
        hList=CreateWindowW(L"LISTBOX",0,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,20,130,440,220,h,(HMENU)100,0,0);
        hSlider=CreateWindowW(TRACKBAR_CLASSW,0,WS_CHILD|WS_VISIBLE|TBS_NOTICKS|TBS_ENABLESELRANGE,20,360,440,30,h,(HMENU)101,0,0); SendMessage(hSlider,TBM_SETRANGE,1,MAKELPARAM(0,1000));
        hTime=CreateWindowW(L"STATIC",L"0:00 / 0:00",WS_CHILD|WS_VISIBLE|SS_CENTER,190,390,100,20,h,0,0,0);
        int y=420;
        hBtnLoop=CreateWindowW(L"BUTTON",L"🔁 Off",WS_CHILD|WS_VISIBLE,20,y,80,30,h,(HMENU)8,0,0);
        CreateWindowW(L"BUTTON",L"⏮",WS_CHILD|WS_VISIBLE,140,y,50,30,h,(HMENU)2,0,0);
        hBtnPlay=CreateWindowW(L"BUTTON",L"▶",WS_CHILD|WS_VISIBLE,200,y,80,30,h,(HMENU)1,0,0);
        CreateWindowW(L"BUTTON",L"⏭",WS_CHILD|WS_VISIBLE,290,y,50,30,h,(HMENU)3,0,0);
        CreateWindowW(L"BUTTON",L"📂",WS_CHILD|WS_VISIBLE,410,y,50,30,h,(HMENU)4,0,0);
        EnumChildWindows(h,[](HWND c,LPARAM f){SendMessage(c,WM_SETFONT,f,1);return TRUE;},(LPARAM)f);
        SetTimer(h,1,200,0);
    }
    if(m==WM_COMMAND) {
        int id=LOWORD(w);
        if(id==1) ToggleP(); if(id==2) PrevT(); if(id==3) NextT(); if(id==4) OpenD(false);
        if(id==8) { isLooping=!isLooping; SetWindowTextW(hBtnLoop, isLooping?L"🔁 ON":L"🔁 Off"); }
        if(id==100 && HIWORD(w)==LBN_DBLCLK) { int i=SendMessage(hList,LB_GETCURSEL,0,0); if(i!=-1) PlayTrack(i); }
    }
    if(m==WM_HSCROLL && (HWND)l==hSlider && LOWORD(w)==TB_ENDTRACK) {
        if(isPlaying && totalFrames>0) { size_t p=SendMessage(hSlider,TBM_GETPOS,0,0); seekTarget=(size_t)((double)p/1000.0*totalFrames); seekReq=true; }
    }
    if(m==WM_USER_UPDATE_UI) InvalidateRect(h,0,0);
    if(m==WM_USER_NEXT) NextT();
    if(m==WM_TIMER && isPlaying && totalFrames>0 && !seekReq) {
        int p=(int)((double)currentFrame/totalFrames*1000); SendMessage(hSlider,TBM_SETPOS,1,p);
        size_t s=currentFrame/currentSampleRate, t=totalFrames/currentSampleRate;
        wchar_t b[64]; wsprintfW(b,L"%d:%02d / %d:%02d", (int)s/60,(int)s%60, (int)t/60,(int)t%60); SetWindowTextW(hTime,b);
    }
    if(m==WM_PAINT) { PAINTSTRUCT p; HDC dc=BeginPaint(h,&p); RECT r; GetClientRect(h,&r); DrawUI(dc,r); EndPaint(h,&p); }
    if(m==WM_DESTROY) { StopAll(); PostQuitMessage(0); }
    return DefWindowProcW(h,m,w,l);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput g; ULONG_PTR t; GdiplusStartup(&t,&g,NULL);
    InitCommonControls();
    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.lpszClassName=L"V"; wc.hbrBackground=NULL; wc.hCursor=LoadCursor(0,IDC_ARROW);
    RegisterClassExW(&wc);
    CreateWindowW(L"V",L"Velox Player",WS_VISIBLE|WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX,200,200,500,520,0,0,h,0);
    MSG m; while(GetMessage(&m,0,0,0)) DispatchMessage(&m);
    GdiplusShutdown(t); return 0;
}