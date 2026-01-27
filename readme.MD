# Velox Codec

A high-performance, lossless audio codec with advanced compression techniques for WAV files. The Velox codec uses neural predictors, Linear Predictive Coding (LPC), long-term prediction (LTP), and entropy encoding to achieve efficient compression while maintaining perfect audio reconstruction.

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
9. **VeloxPlayerGUI.cpp** - Windows GUI player application

## Building

### Requirements

- g++ compiler with C++11 or later
- Windows OS (currently Windows-only)

### Command-Line Tool

Compile the Velox encoder/decoder:

```powershell
g++ main.cpp -o velox -O3
```

### GUI Player

Compile the Velox player GUI (Windows only):

```powershell
g++ VeloxPlayerGUI.cpp -o VeloxPlayer -O3 -mwindows -lcomdlg32 -lole32 -lshell32 -lwinmm -lgdi32 -lcomctl32 -static
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

- **Windows**: Fully supported (both CLI and GUI)
- **Linux/macOS**: Not currently supported

The GUI player (`VeloxPlayerGUI`) requires Windows-specific APIs (Windows.h, winmm.lib, GDI+, etc.) and is not compatible with other platforms. However, the CLI encoder/decoder tools may be portable with modification.

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

### Key Algorithms

- **Fixed-Point Arithmetic**: 12-bit fixed-point representation for DSP operations
- **Zig-Zag Encoding**: Efficient signed integer encoding
- **Exponential Golomb Codes**: Variable-length encoding of prediction residuals
- **Thread Pool**: Parallel processing of stereo channels and blocks

## Development Status

⚠️ **EXPERIMENTAL**: This codec is actively under development with evolving compression algorithms. It is not officially released and stability may vary. Use with caution in production environments.

## Performance Tips

1. Use `-O3` optimization flag during compilation for best performance
2. For very large files, the multi-threaded implementation provides better throughput
3. The codec adapts automatically to audio characteristics; no parameter tuning required
4. Compression ratio varies by content type (speech, music, silence)

## Example Workflow

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

## License

[License information pending]