# Custom AI Notetaker - Windows Setup Guide

## 📦 Dependencies Downloaded

All dependencies have been manually downloaded and configured for Windows:

### ✅ Successfully Downloaded:

1. **PortAudio** (v19.7.0) - Cross-platform audio I/O
   - Location: `deps/portaudio/portaudio/`
   - Source: `pa_stable_v190700_20210406.tgz`

2. **libsndfile** (v1.2.2) - Audio file reading/writing
   - Location: `deps/libsndfile/libsndfile-1.2.2-win64/`
   - Precompiled Windows binaries included

3. **CURL** (v8.15.0) - HTTP client library
   - Location: `deps/curl/curl-8.15.0_7-win64-mingw/`
   - Precompiled Windows binaries included

## 🔧 Project Structure

```
custom-ai-notetaker/
├── CMakeLists.txt          # ✅ Updated for local dependencies
├── src/main.cpp            # ✅ Full implementation ready
├── models/
│   ├── ggml-base.en.bin        # ✅ Whisper model (142MB)
│   └── ggml-small.en-tdrz.bin  # ✅ TinyDiarize model (465MB) - Default
├── deps/                  # ✅ Manual dependencies
│   ├── portaudio/
│   ├── libsndfile/
│   └── curl/
└── third_party/
    └── whisper.cpp/       # ✅ Git submodule
```

## 🚀 Building the Project

### Prerequisites:
- Visual Studio 2019/2022 with C++ support
- CMake 3.20+
- Git

### Build Steps:

1. **Create build directory:**
   ```powershell
   mkdir build
   cd build
   ```

2. **Generate build files:**
   ```powershell
   cmake ..
   ```

3. **Build the project:**
   ```powershell
   cmake --build . --config Release
   ```

## ▶️ Running the Application

1. **Set your OpenAI API key:**

   **Option A: Using .env file (Recommended - Secure):**
   ```powershell
   # The .env file is already created with your API key
   # Just make sure it exists and contains:
   # OPENAI_API_KEY=your_key_here
   ```
   
   **Option B: Using environment variable:**
   ```powershell
   $env:OPENAI_API_KEY="sk-your-api-key-here"
   ```

2. **Run the application:**

   **For capturing system audio with speaker diarization (recommended):**
   ```powershell
   .\Release\meetnotes.exe --loopback --seconds 600
   ```
   
   **For traditional microphone recording with speaker diarization:**
   ```powershell
   .\Release\meetnotes.exe --seconds 600
   ```
   
   **Using non-diarization model:**
   ```powershell
   .\Release\meetnotes.exe --loopback --model ../models/ggml-base.en.bin --seconds 600
   ```

## 📋 Available Options

- `--model`: Path to Whisper model file (default: models/ggml-small.en-tdrz.bin with diarization)
- `--seconds`: Recording duration in seconds (default: 600)
- `--loopback`: **NEW!** Use WASAPI loopback to capture system audio (Windows only)
- `--device`: Audio input device substring (PortAudio mode only)
- `--out`: Output audio file path

## 🎤 **WASAPI Loopback Mode**

The new `--loopback` option uses Windows WASAPI to capture the exact audio playing through your speakers. Perfect for:

- **Recording online meetings** (Zoom, Teams, Google Meet)
- **Capturing webinars or presentations**
- **Recording any system audio**
- **Perfect quality** without background noise or echo

## 🎙️ **Speaker Diarization (TinyDiarize)**

By default, the application now uses the **TinyDiarize model** (`ggml-small.en-tdrz.bin`) which provides:

- **Speaker identification**: Automatically detects "who spoke when"
- **Speaker segmentation**: Separates transcript by speaker
- **Professional output**: Clear speaker attribution in transcripts

**Example Output:**
```
Speaker 1: Welcome everyone to today's meeting. Let's start by reviewing the quarterly results.
Speaker 2: Thank you for having me. I'd like to present our sales figures first.
Speaker 1: That sounds great. Please go ahead.
```

**Example:**
```powershell
# Record a 10-minute meeting with WASAPI loopback and speaker diarization
.\Release\meetnotes.exe --loopback --seconds 600 --out meeting.wav
```

## 🔧 CMake Configuration

The CMakeLists.txt has been updated to automatically find and link the manually downloaded dependencies:

- **PortAudio**: Headers and libraries from `deps/portaudio/`
- **libsndfile**: Precompiled Windows binaries from `deps/libsndfile/`
- **CURL**: Precompiled Windows binaries from `deps/curl/`
- **Whisper.cpp**: Built from submodule in `third_party/`

## 🧹 Git Ignore

The `.gitignore` has been configured to:
- ✅ Keep extracted dependency folders
- ❌ Exclude dependency archives (.zip, .tgz)
- ❌ Exclude build artifacts
- ❌ Exclude large model files (download separately)

## 🎯 Next Steps

1. Build the project using the steps above
2. Test with a short recording first
3. Adjust model size based on your needs:
   - `tiny.en` (75MB) - Fastest, basic quality, no diarization
   - `base.en` (142MB) - Good balance, no diarization  
   - `small.en-tdrz` (465MB) - Best for meetings ⭐ (current, with diarization)
   - `medium.en` (1.5GB) - Highest quality, no diarization

## 🐛 Troubleshooting

**Build Issues:**
- Ensure Visual Studio C++ tools are installed
- Check that all dependency paths exist in CMake
- Try building whisper.cpp separately first

**Runtime Issues:**
- Verify OpenAI API key is set (check .env file or environment variable)
- Check that model file exists and is accessible
- Ensure audio input device is available

**API Key Issues:**
- Make sure .env file exists in project root
- Verify .env file contains: `OPENAI_API_KEY=your_actual_key`
- Check that .env file is not corrupted or has extra spaces

**Library Issues:**
- Make sure DLL files are in PATH or copy them to executable directory
- Check that all dependency versions match your system architecture (x64)

## 📚 Dependencies Details

| Library | Version | Purpose | Status |
|---------|---------|---------|---------|
| PortAudio | 19.7.0 | Audio I/O | ✅ Source downloaded |
| libsndfile | 1.2.2 | Audio files | ✅ Precompiled |
| CURL | 8.15.0 | HTTP client | ✅ Precompiled |
| Whisper.cpp | Latest | Speech-to-text | ✅ Git submodule |

All dependencies are now ready for building on Windows! 🎉
