#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <windows.h>
#include <conio.h> // Để bắt phím điều khiển
#include <iomanip>
#include "VeloxCore.h"

#pragma comment(lib, "winmm.lib")

// --- CÁC HÀM GIAO DIỆN (UI) ---

void SetColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

void HideCursor() {
    HANDLE consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info;
    info.dwSize = 100;
    info.bVisible = FALSE;
    SetConsoleCursorInfo(consoleHandle, &info);
}

void Gotoxy(int x, int y) {
    COORD coord;
    coord.X = x;
    coord.Y = y;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

std::string FormatTime(long ms) {
    int total_seconds = ms / 1000;
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;
    char buffer[10];
    sprintf(buffer, "%02d:%02d", minutes, seconds);
    return std::string(buffer);
}

// Hàm gửi lệnh MCI đơn giản
void SendMCI(std::string cmd, char* buffer = nullptr, int bufSize = 0) {
    mciSendStringA(cmd.c_str(), buffer, bufSize, NULL);
}

// --- LOGIC CHÍNH ---

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: velox_player music.vlx\n";
        return 1;
    }

    // 1. Setup UI
    HideCursor();
    system("cls");
    SetColor(11); // Cyan Color
    std::cout << R"(
 __      __   _              _____  _                        
 \ \    / /  | |            |  __ \| |                       
  \ \  / /___| | _____  __  | |__) | | __ _ _   _  ___ _ __  
   \ \/ // _ \ |/ _ \ \/ /  |  ___/| |/ _` | | | |/ _ \ '__| 
    \  /|  __/ | (_) >  <   | |    | | (_| | |_| |  __/ |    
     \/  \___|_|\___/_/\_\  |_|    |_|\__,_|\__, |\___|_|    
                                             __/ |           
                                            |___/            
    )" << "\n";
    SetColor(7); // White

    const char* filename = argv[1];
    std::string tempWav = "temp_playback_buffer.wav";

    // 2. Giải mã (Decode Phase)
    std::cout << "  [*] Loading & Decoding bit-stream...";
    
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) { std::cerr << " Error!\n"; return 1; }

    char magic[4]; in.read(magic, 4);
    uint32_t s;
    in.read((char*)&s, 4); std::vector<uint8_t> header(s); in.read((char*)header.data(), s); // Header
    in.read((char*)&s, 4); in.seekg(s, std::ios::cur); // Skip Footer
    uint8_t pad; in.read((char*)&pad, 1); // Skip Pad Flag

    std::vector<uint8_t> compressed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto pcm = VeloxCodec::DecodeStereo(compressed.data(), compressed.size());

    // 3. Ghi file tạm để MCI xử lý (Cần thiết cho Seek/Pause)
    {
        std::ofstream out(tempWav, std::ios::binary);
        out.write((char*)header.data(), header.size());
        out.write((char*)pcm.data(), pcm.size() * 2);
    }
    std::cout << " Done!\n";
    Sleep(500); // Hiệu ứng chờ

    // 4. Khởi động Audio Engine
    SendMCI("close all");
    std::string openCmd = "open \"" + tempWav + "\" type waveaudio alias veloxAudio";
    SendMCI(openCmd);
    SendMCI("play veloxAudio");

    // Lấy độ dài bài hát
    char durBuf[128];
    SendMCI("status veloxAudio length", durBuf, 128);
    long totalDuration = atol(durBuf);

    bool isPaused = false;
    bool running = true;

    // 5. Vòng lặp giao diện (Game Loop)
    system("cls"); // Xóa màn hình loading
    
    while (running) {
        // --- VẼ UI ---
        Gotoxy(0, 0);
        SetColor(11);
        std::cout << "  NOW PLAYING: ";
        SetColor(15);
        std::cout << filename << "\n\n";

        // Lấy vị trí hiện tại
        char posBuf[128];
        SendMCI("status veloxAudio position", posBuf, 128);
        long currentPos = atol(posBuf);

        // Vẽ thanh tiến trình
        float percent = (float)currentPos / totalDuration;
        int barLength = 50;
        int progress = (int)(percent * barLength);

        std::cout << "  [";
        for (int i = 0; i < barLength; i++) {
            if (i < progress) {
                SetColor(10); // Green
                std::cout << "=";
            } else if (i == progress) {
                SetColor(14); // Yellow (Head)
                std::cout << ">";
            } else {
                SetColor(8); // Gray
                std::cout << "-";
            }
        }
        SetColor(7);
        std::cout << "]  ";
        
        // Vẽ thời gian
        std::cout << FormatTime(currentPos) << " / " << FormatTime(totalDuration) << "   \n";

        // Vẽ trạng thái
        std::cout << "\n  STATUS: ";
        if (isPaused) {
            SetColor(14); std::cout << "[ PAUSED ] ";
        } else {
            SetColor(10); std::cout << "[ PLAYING ]";
        }

        // Hướng dẫn
        SetColor(8);
        std::cout << "\n\n  ----------------------------------";
        std::cout << "\n  [SPACE] Pause/Resume   [ESC] Quit";
        std::cout << "\n  [<] Rewind 5s          [>] Forward 5s";

        // Kiểm tra kết thúc bài
        if (currentPos >= totalDuration && !isPaused) {
            running = false;
        }

        // --- XỬ LÝ PHÍM BẤM ---
        if (_kbhit()) {
            char key = _getch();
            if (key == 32) { // Space
                if (isPaused) {
                    SendMCI("resume veloxAudio");
                    isPaused = false;
                } else {
                    SendMCI("pause veloxAudio");
                    isPaused = true;
                }
            }
            else if (key == 27) { // ESC
                running = false;
            }
            else if (key == -32) { // Arrow keys header
                char arrow = _getch();
                long newPos = currentPos;
                if (arrow == 75) newPos -= 5000; // Left
                if (arrow == 77) newPos += 5000; // Right
                
                if (newPos < 0) newPos = 0;
                if (newPos > totalDuration) newPos = totalDuration;

                std::string seekCmd = "play veloxAudio from " + std::to_string(newPos);
                if (isPaused) seekCmd = "seek veloxAudio to " + std::to_string(newPos); // Nếu đang pause thì chỉ seek thôi
                
                SendMCI(seekCmd);
            }
        }

        Sleep(100); // 10 FPS UI Update
    }

    // 6. Dọn dẹp
    SendMCI("close veloxAudio");
    remove(tempWav.c_str()); // Xóa file tạm
    
    system("cls");
    SetColor(7);
    std::cout << "Playback finished. Goodbye!\n";
    return 0;
}