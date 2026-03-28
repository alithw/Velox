# Velox Codec

A high-performance, lossless audio codec with advanced compression techniques for WAV files. The Velox codec uses neural predictors, Linear Predictive Coding (LPC), long-term prediction (LTP), and entropy encoding to achieve efficient compression while maintaining perfect audio reconstruction.

Includes streaming server and client with intelligent bandwidth-aware buffering for network streaming of Velox-compressed audio.

## Overview

Velox is a sophisticated audio compression system designed to minimize file size while ensuring bit-perfect lossless reconstruction. The codec implements several advanced signal processing techniques:

- **Neural Predictor**: Adaptive prediction using learned weights
- **Linear Predictive Coding (LPC)**: 12th-order LPC with dynamic coefficient calculation
- **Long-Term Prediction (LTP)**: Pattern matching across audio history
- **Entropy Encoding**: Efficient bit-level encoding of residual signals
- **LSB Shifting**: Automatic detection and optimization of low-bit information
- **Metadata Support**: Vorbis-style metadata tags and cover art support
- **Float Detection**: Intelligent demoting of float samples to integer representation
- **Multi-threaded Processing**: Parallel encoding/decoding for improved performance
- **AIFF Support**: Encode/decode AIFF files in addition to WAV
- **Smart Metadata Extraction**: Automatic extraction of ID3v2 tags and RIFF INFO from source files
- **Network Streaming**: Streaming server with range request support for client-side playback
- **Adaptive Buffering**: Smart bandwidth-aware buffering for optimized streaming

## Architecture

### Core Components

1. **VeloxCore.h** - Main compression/decompression engine with neural predictors and LPC
2. **VeloxFormat.h** - Audio format detection and conversion (PCM, Float, etc.)
3. **VeloxArch.h** - Architecture definitions and fixed-point math utilities
4. **VeloxAdvanced.h** - Advanced optimization techniques (silence detection, LTP)
5. **VeloxEntropy.h** - Bitstream I/O and entropy coding
6. **VeloxMetadata.h** - Vorbis-style metadata and cover art handling
7. **VeloxThreads.h** - Thread pool for parallel processing
8. **main.cpp** - Command-line encoder/decoder utility
9. **velox_player_main.cpp** - Qt 6 GUI entry point
10. **VeloxQtPlayerWindow.cpp** - Qt 6 GUI window
11. **VeloxQtPlayerEngine.cpp** - Qt 6 playback engine
12. **VeloxPlayerGUI.cpp** - Legacy Win32 GUI player (deprecated)
13. **VeloxServer.cpp** - Network streaming server
14. **VeloxStreamClient.cpp** - Network streaming client with GUI

## Building

### Requirements

- C++17 compiler (Qt 6 requires C++17)
- Meson + Ninja
- Qt 6 (Widgets + Multimedia) for the GUI player
- MSYS2 (MinGW or Clang toolchain) recommended on Windows
- Windows Sockets 2 (Winsock2) for streaming server/client
- Windows Multimedia API for audio output

Install on MSYS2 (UCRT64 example):

> [!WARNING]
> Note that MSVC is not supported yet (uses GCC/Clang builtins). MSYS2 GCC/Clang is supported.

```powershell
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain meson ninja mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-multimedia
```

Linux (Debian/Ubuntu example):

```bash
sudo apt install build-essential meson ninja-build qt6-base-dev qt6-multimedia-dev
```

macOS (Homebrew example):

```bash
brew install qt meson ninja
```

### Meson Build (recommended)

Configure and build:

```powershell
meson setup build --buildtype=release
meson compile -C build
```

Outputs:

- CLI: `build/velox`
- GUI: `build/velox_player`

Common options:

- Disable GUI: `-Dbuild_gui=false` (useful if Qt 6 is not available)
- Disable CLI: `-Dbuild_cli=false`
- MinGW static link: `-Dstatic_mingw=true`

Example (CLI only):

```powershell
meson setup build --buildtype=release -Dbuild_gui=false
meson compile -C build
meson install -C build
```

### Manual g++ Build (legacy)

Command-line tool:

```powershell
g++ main.cpp -o velox -O3
```

Legacy Win32 GUI player (Windows only):

```powershell
g++ VeloxPlayerGUI.cpp -o VeloxPlayer -O3 -mwindows -lcomdlg32 -lole32 -lshell32 -lwinmm -lgdi32 -lcomctl32 -static
```

### Streaming Server Build (Windows only)

Build the VeloxServer (Windows):

```powershell
g++ VeloxServer.cpp -o VeloxServer -O3 -lws2_32 -static
```

### Streaming Client Build

Build the VeloxStreamClient (Windows):

```powershell
g++ VeloxStreamClient.cpp -o VeloxStreamClient -O3 -mwindows -lws2_32 -lwinmm -lgdi32 -lcomctl32 -static
```

## Usage

### Encoding

Compress a WAV or AIFF file to Velox format with optional metadata:

```powershell
velox -c input.wav output.vlx [Artist] [Title]
velox -c input.aif output.vlx [Artist] [Title]
```

**Parameters:**
- `-c`: Encode mode
- `input.wav/aif`: Source WAV or AIFF file
- `output.vlx`: Output Velox file
- `[Artist]` (optional): Artist metadata (auto-extracted if not provided)
- `[Title]` (optional): Title metadata (auto-extracted if not provided)

**Example:**
```powershell
velox -c song.wav song.vlx "John Doe" "My Song"
velox -c song.aif song.vlx                           # Auto-extract metadata from AIFF
```

### Decoding

Decompress a Velox file back to WAV format:

```powershell
velox -d input.vlx output.wav
```

**Parameters:**
- `-d`: Decode mode
- `input.vlx`: Source Velox file
- `output.wav`: Output WAV file

**Example:**
```powershell
velox -d song.vlx restored.wav
```

## Network Streaming

Velox provides client-server streaming capabilities for efficient audio streaming over networks with adaptive buffering.

### VeloxServer Setup

VeloxServer acts as a streaming server that hosts Velox-compressed audio files and handles client requests with intelligent range-request support for seeking and streaming.

#### Server Directory Structure

Create the following directory and place your Velox files there:

```
veloxcodec/
├── VeloxServer.exe
└── music/
    ├── song1.vlx
    ├── song2.vlx
    └── song3.vlx
```

#### Starting the Server

```powershell
# Run from the directory containing the VeloxServer executable
cd veloxcodec
./VeloxServer.exe
```

**Output:**
```
======================================
|   VELOX STREAMING SERVER v3.0      |
|   - Smart Range Request enabled    |
|   - Port: 6781    |
======================================

[Server] Scanning ./music/ directory...
[Server] Hosted: song1.vlx (12500 KB)
[Server] Hosted: song2.vlx (8750 KB)
[Server] Hosted: song3.vlx (15000 KB)
[Server] Database ready. Total tracks: 3
[Server] Server is running. Waiting for connections...
```

#### Server Commands

The server supports two main commands:

1. **LIST Command** - Get the playlist
   - Request: `LIST`
   - Response: Pipe-separated format: `ID|Filename|FileSize\n`

2. **GET Command** - Range request for streaming/seeking
   - Request: `GET <TrackID> <OffsetBytes> <LengthBytes>`
   - Response: Raw audio data

#### Server Features

- **Adaptive Buffering**: Only buffers up to 5MB ahead of playback position (configurable via `MAX_BUFFER_AHEAD`)
- **Range Requests**: Supports arbitrary byte ranges for seeking within tracks
- **Multiple Clients**: Handles multiple concurrent connections
- **Timeout Management**: 5-second timeout per client command
- **Safe Data Transmission**: Custom protocol with 4-byte length prefix for reliable delivery

### VeloxStreamClient Setup

VeloxStreamClient is a Windows GUI application that streams Velox files from a VeloxServer with real-time playback and seeking.

#### Client Directory Structure

```
veloxcodec/
├── VeloxStreamClient.exe
├── VeloxCore.h
├── VeloxMetadata.h
└── ... (other header files)
```

#### Starting the Client

```powershell
# Run the client executable
./VeloxStreamClient.exe
```

#### Client GUI Features

- **Playlist Display**: Shows all available tracks from the server
- **Playback Controls**:
  - Play/Pause button (toggles between play and pause)
  - Previous track button (<<)
  - Next track button (>>)
- **Seeking**: Click on the progress bar to seek to any position in the track
- **Real-time Display**:
  - Current track title
  - Playback status (Connecting, Converting, Playing, Seeking, Buffering)
  - Download progress (light gray)
  - Playback progress (green)
  - Current time and total duration display

#### Client Configuration

Default connection parameters:

```cpp
server.Connect("127.0.0.1", "6781")  // Server IP and port
MAX_BUFFER_AHEAD = 7 * 1024 * 1024   // 7MB ahead of playback
```

To modify these, edit the values in `VeloxStreamClient.cpp`:

**Change Server Address:**
```cpp
// In WndProc, WM_CREATE section
if (server.Connect("192.168.1.100", "6781")) {  // Change to your server IP
```

**Change Buffer Size:**
```cpp
// Near the top of the file with other globals
const size_t MAX_BUFFER_AHEAD = 10 * 1024 * 1024;  // 10MB instead of 7MB
```

Then rebuild:
```powershell
g++ VeloxStreamClient.cpp -o VeloxStreamClient -O3 -mwindows -lws2_32 -lwinmm -lgdi32 -lcomctl32 -static
```

#### Client Operation

1. **Start VeloxServer** on the host machine
2. **Run VeloxStreamClient** on the client machine
3. **Connect**: Client automatically fetches the playlist on startup
4. **Select Track**: Double-click a track in the playlist to start playback
5. **Playback Controls**:
   - Click Play/Pause to control playback
   - Click << to go to previous track
   - Click >> to go to next track
   - Click anywhere on the progress bar to seek

#### Client Performance Tips

- **Local Network Streaming**: Best results on local network (LAN)
- **Bandwidth Requirements**: Varies by audio format, typically 150-500 kbps for lossless Velox
- **Buffer Management**: The client automatically adjusts buffering based on network speed
- **Concurrent Streaming**: Multiple clients can stream simultaneously from one server

### Example Setup Workflow

#### Terminal 1 - Start the Server

```powershell
cd C:\Code\Personal\veloxcodec
.\VeloxServer.exe
```

#### Terminal 2 - Start the Client

```powershell
cd C:\Code\Personal\veloxcodec
.\VeloxStreamClient.exe
```

#### Expected Output

**Server Terminal:**
```
[Server] Client connected. Socket ID: 2460
[Server] Client connected. Socket ID: 2464
[Server] Client disconnected.
```

**Client Window:**
- Playlist populated with server's tracks
- Status shows "Connected!"
- Ready for playback

## Verification

### Data Integrity

Since Velox is lossless, the reconstructed audio should be bit-perfect identical to the original. Verify this using:

**Method 1: MD5 Hash Comparison**

```powershell
# Calculate hash of original file
$original_hash = (Get-FileHash original.wav -Algorithm MD5).Hash

# Calculate hash of restored file
$restored_hash = (Get-FileHash restored.wav -Algorithm MD5).Hash

# Compare hashes
if ($original_hash -eq $restored_hash) {
    Write-Host "✓ Files are identical (perfect reconstruction)"
} else {
    Write-Host "✗ Files differ - codec error detected"
}
```

**Method 2: Binary Comparison**

```powershell
# Using Windows fc (File Compare) command
cmd /c fc /b original.wav restored.wav
# Output: "FC: no differences encountered" = identical

# Or using diff
diff original.wav restored.wav
```

### Compression Ratio

Check the compression effectiveness:

```powershell
# Display file sizes
$files = Get-ChildItem *.wav, *.vlx | Select-Object Name, @{Name="Size(MB)";Expression={[math]::Round($_.Length / 1MB, 2)}}
$files | Format-Table

# Calculate compression ratio
$original_size = (Get-Item original.wav).Length
$compressed_size = (Get-Item output.vlx).Length
$ratio = [math]::Round(($compressed_size / $original_size) * 100, 2)
Write-Host "Compression ratio: $ratio%"
```

## Platform Support

- **Windows**: Fully supported (CLI + Qt GUI + Streaming Server/Client)
- **Linux/macOS**: Supported with the Qt 6 GUI and the CLI (streaming components require Winsock2 porting)

The legacy Win32 GUI (`VeloxPlayerGUI.cpp`) is Windows-only. The Qt 6 GUI (`velox_player`) is cross-platform. The streaming server and client (`VeloxServer.cpp`, `VeloxStreamClient.cpp`) are Windows-only and require Winsock2.

## File Format

### VLX File Structure

```
[VeloxHeader]
  magic: "VELX" (4 bytes)
  version: 2 bytes
  sample_rate: 4 bytes
  channels: 2 bytes
  bits_per_sample: 2 bytes
  format_code: 2 bytes (1=PCM, 3=Float)
  total_samples: 8 bytes
  header_blob_size: 4 bytes
  footer_blob_size: 4 bytes

[Compressed Audio Data]
  Variable-length encoded frames

[Metadata Block] (optional)
  Vorbis-style metadata with tags
  Optional cover art
```

## Technical Details

### Encoding Process

1. **Audio Format Detection**: Automatically detects WAV or AIFF format
2. **Metadata Extraction**: Intelligently extracts tags from source files:
   - **ID3v2 Tags** (WAV/MP3): Reads TIT2, TPE1, TALB frames
   - **RIFF INFO Chunks** (WAV): Reads IART, INAM metadata
   - **AIFF Comments**: Reads ANNO and other comment fields
   - **Manual Override**: Command-line arguments override auto-detected metadata
3. **Format Detection**: Identifies float vs. integer samples and attempts lossless float demotion
4. **LSB Analysis**: Detects and separates low-order bits for optimization
5. **LPC Calculation**: Computes 12-order LPC coefficients using Levinson-Durbin algorithm
6. **Neural Prediction**: Adapts prediction weights based on prediction errors
7. **LTP Matching**: Searches audio history for repeating patterns
8. **Entropy Encoding**: Variable-length encodes residual values
9. **Metadata Embedding**: Attaches Vorbis-style tags and optional cover art

### Streaming Architecture

The streaming components use a three-threaded architecture for optimal performance:

1. **Download Thread**: Manages network communication with the server
   - Implements smart buffering (only buffers up to 5-7MB ahead)
   - Respects bandwidth optimization
   - Handles range requests for seeking

2. **Decoder Thread**: Decodes compressed audio from RAM
   - Waits for sufficient header data before starting
   - Buffers decoded PCM samples in real-time
   - Handles seek requests by fast-forwarding through the stream

3. **Output Thread**: Writes decoded audio to system audio output
   - Runs at high priority for low-latency playback
   - Manages Windows waveOut API buffers
   - Triggers next-track events when current track finishes

### Key Algorithms

- **Fixed-Point Arithmetic**: 12-bit fixed-point representation for DSP operations
- **Zig-Zag Encoding**: Efficient signed integer encoding
- **Exponential Golomb Codes**: Variable-length encoding of prediction residuals
- **Thread Pool**: Parallel processing of stereo channels and blocks
- **Ring Buffer**: Lock-free-like synchronized queue between decode and output threads

## Development Status

✅ **STABLE**: This codec is production-ready with proven stability and reliable compression algorithms. Extensively tested across various audio formats and network conditions. Safe for production use.

## Compression Performance Comparison

### Music Files: FLAC vs VLX

Comparison of industry-standard FLAC (maximum compression) with Velox VLX compression on 5 music files from our test:

| File | FLAC Size (MB) | VLX Size (MB) | Compression Ratio | Notes |
|------|----------------|---------------|-------------------|-------|
| bloom | 60.265 | 60.827 | 100.93% | Slightly larger than FLAC |
| honey | 27.605 | 27.229 | 98.64% | **2.4% smaller than FLAC** |
| kyd | 43.097 | 42.202 | 97.92% | **2.1% smaller than FLAC** |
| rain | 26.921 | 26.041 | 96.73% | **3.3% smaller than FLAC** |
| sky | 29.508 | 29.420 | 99.70% | Nearly identical to FLAC |
| **Average** | — | — | **98.78%** | **Competitive with FLAC** |

### Format Test Files: WAV vs VLX

Compression performance on specialized audio format tests from `osoundtest/wavt/` and `osoundtest/vlx_encoded/`:

| Format Type | WAV Size (MB) | VLX Size (MB) | Compression Ratio | Efficiency |
|-------------|---------------|---------------|--------------------|------------|
| 24-bit Fixed Point | 1.861 | 0.687 | 36.94% | **Excellent - 63% reduction** |
| 32-bit Float | 2.479 | 1.335 | 53.86% | **Very Good - 46% reduction** |
| PCM 32-bit | 2.250 | 0.830 | 36.90% | **Excellent - 63% reduction** |
| Single 44kHz | 2.250 | 1.082 | 48.08% | **Very Good - 52% reduction** |
| 384kHz Hi-Res | 31.858 | 8.717 | 27.36% | **Outstanding - 73% reduction** |
| 768kHz Ultra Hi-Res | 240.468 | 147.256 | 61.24% | **Good - 39% reduction** |
| **Average** | — | — | **44.06%** | **Highly efficient across formats** |

### Key Insights

1. **Music vs Format Tests**: VLX compression varies significantly based on audio format
   - Standard formats: 97-101% of FLAC size (competitive for lossy comparison)
   - Lossless format tests: 27-64% of original WAV size (excellent compression)

2. **Strengths**:
   - **Outstanding** on 24-bit fixed and PCM formats (63% reduction)
   - **Best performance** on high-resolution audio (384kHz: 73% reduction)
   - Handles float-point formats efficiently despite precision complexity

3. **Performance Factors**:
   - Hi-resolution audio (384kHz+) achieves excellent compression
   - Fixed-point formats compress better than float-point
   - Standard music streaming formats remain competitive with FLAC

## Performance Tips

1. Use `-O3` optimization flag during compilation for best performance
2. For very large files, the multi-threaded implementation provides better throughput
3. The codec adapts automatically to audio characteristics; no parameter tuning required
4. Compression ratio varies by content type (speech, music, silence)
5. For network streaming:
   - Use gigabit Ethernet or modern WiFi for optimal performance
   - Place the server on a stable, well-connected machine
   - Adjust `MAX_BUFFER_AHEAD` based on network conditions

## Example Workflow

### Local Encoding and Playback

```powershell
# 1. Encode a WAV file with auto-detected metadata
velox -c album_track.wav album_track.vlx

# 2. Encode an AIFF file with manual metadata override
velox -c album_track.aif album_track.vlx "Artist Name" "Track Title"

# 3. Verify compression
ls album_track.wav, album_track.vlx | Format-Table Name, @{Name="Size(MB)";Expression={$_.Length / 1MB}}

# 4. Decode for verification
velox -d album_track.vlx album_track_restored.wav

# 5. Verify integrity
(Get-FileHash album_track.wav -Algorithm MD5).Hash
(Get-FileHash album_track_restored.wav -Algorithm MD5).Hash
```

### Network Streaming Setup

```powershell
# 1. Prepare server media directory
mkdir music
# Copy your *.vlx files to the music/ directory

# 2. Start the server (Terminal 1)
C:\path\to\VeloxServer.exe

# 3. Start the client (Terminal 2)
C:\path\to\VeloxStreamClient.exe

# 4. Client window appears - select tracks from the playlist and enjoy!
```

## Troubleshooting

### Streaming Issues

**Client shows "Server Offline":**
- Verify server is running
- Check firewall settings on server machine
- Verify network connectivity between client and server
- Check that port 6781 is not blocked

**Playback stutters or buffers frequently:**
- Check network bandwidth and latency
- Increase `MAX_BUFFER_AHEAD` value in client code for more buffering
- Move files to a local network location
- Check for other network activity

**Seeking is slow:**
- Network decode position calculation depends on network speed
- May need to wait for buffer to catch up to seek position
- Try seeking to nearby positions first

### Compilation Issues

**"winsock2.h not found":**
- Part of Windows SDK, usually included with MinGW
- If missing, reinstall mingw-w64 toolchain

**comctl32 linking error:**
- Ensure `-lcomctl32` is included in compilation flags
- Add `#pragma comment(lib, "comctl32.lib")` in source if using MSVC

## License

This project is released under the [MIT License](LICENSE).
