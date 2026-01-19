## Cách kiểm tra:

### 1. Biên dịch (Build)
Compile file `velox.exe`:

```powershell
g++ main.cpp -o velox -O3
```

### 2. Nén (Encode) & Giải nén (Decode)
Chạy nén file gốc ra `.vlx` và giải nén ngược lại ra `.wav` để kiểm tra.

```powershell
# Nén 
Encode: velox -c input.wav output.vlx [Artist] [Title] #Ghi Metadata

# Giải nén 
Decode: velox -d input.vlx output.wav
```

### 3. Kiểm tra toàn vẹn dữ liệu (Verification)

**Cách 1: Kiểm tra bằng mã băm MD5**

```powershell
# Tính Hash file gốc
Get-FileHash <file so sánh.wav> -Algorithm MD5

# Tính Hash file phục hồi
Get-FileHash restored.wav -Algorithm MD5
```

**Cách 2: So sánh**
Lệnh này sẽ so sánh từng byte. Nếu không hiện ra thông báo gì (hoặc báo `FC: no differences encountered`), tức là 2 file giống nhau.

```powershell
# Cách 1: Sử dụng lệnh fc (File Compare) của Windows
cmd /c fc /b <file so sánh.wav> restored.wav

# Cách 2: Sử dụng diff
diff <file so sánh.wav> restored.wav
```

---

### 4. Kiểm tra tỷ lệ nén


```powershell
# Xem kích thước các file
ls honey.vlx, restored.wav, <file so sánh.wav> | Select-Object Name, @{Name="Size(MB)";Expression={$_.Length / 1MB}}
```

---

## VeloxPlayerGUI (hiện chỉ hỗ trợ Windows)

### Biên dịch:
```powershell
g++ VeloxPlayerGUI.cpp -o VeloxPlayer -O3 -mwindows -lcomdlg32 -lole32 -lshell32 -lwinmm -lgdi32 -lcomctl32 -static
```

Codec đang trong quá trình phát triển với thuật toán tối ưu mới, chưa phát hành chính thức nên khả năng xảy ra lỗi cao.