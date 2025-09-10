# WASAPI Native Audio Capture Implementation

## Overview

This project has been successfully migrated from using external dependencies (FFmpeg and PortAudio) to a pure native Windows WASAPI implementation for audio capture. The implementation provides high-quality system loopback audio recording without requiring any external audio libraries.

## What Was Changed

### Dependencies Removed
- **FFmpeg**: Previously used for audio device access and format conversion
- **PortAudio**: Previously used for cross-platform audio I/O
- **libcurl**: Replaced with native Windows HTTP APIs (WinINet) for future use
- **libsndfile**: Replaced with native WAV file I/O implementation

### Dependencies Kept
- **whisper.cpp**: Still used for speech recognition and diarization
- **Native Windows APIs**: WASAPI, COM, WinINet

### New Features Implemented

#### 1. Pure WASAPI Loopback Audio Capture
- **Event-driven capture**: Uses `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` for efficient audio processing
- **Shared mode**: Captures the global audio mix at the system's native format
- **MMCSS threading**: Uses "Pro Audio" task priority for low-latency processing
- **Format handling**: Supports float32, int16, and int32 input formats with automatic conversion
- **Buffer flag handling**: Properly handles silent buffers and data discontinuities

#### 2. Native WAV File I/O
- **SimpleWAVWriter**: Direct WAV file writing with proper header management
- **SimpleWAVReader**: Direct WAV file reading for transcription input
- **Format support**: 16-bit PCM WAV files with configurable sample rates and channels

#### 3. Audio Device Enumeration
- **Render device discovery**: Lists available audio output devices
- **Default device selection**: Automatically selects the default render endpoint
- **Device change handling**: Framework for handling device changes (can be extended)

## Key WASAPI Implementation Details

### Initialization Flow
1. `CoInitializeEx` - Initialize COM subsystem
2. `IMMDeviceEnumerator` - Create device enumerator
3. `GetDefaultAudioEndpoint(eRender, eConsole)` - Get default output device
4. `IAudioClient::Initialize` - Initialize in loopback mode with event callback
5. `IAudioClient::SetEventHandle` - Set event for data-ready notifications
6. `IAudioCaptureClient` - Get capture interface

### Capture Loop
1. `WaitForSingleObject` - Wait for audio data event (2-second timeout)
2. `GetNextPacketSize` - Check available packet size
3. `GetBuffer` - Get audio buffer with timestamps and flags
4. Format conversion and storage
5. `ReleaseBuffer` - Release the processed buffer
6. Repeat until recording duration expires

### Audio Format Handling
- **Float32**: Direct copy (most common format)
- **Int16**: Convert to float32 with `/32768.0f` scaling
- **Int32**: Convert to float32 with `/2147483648.0f` scaling
- **Silent buffers**: Fill with zeros when `AUDCLNT_BUFFERFLAGS_SILENT` is set
- **Discontinuities**: Log when `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` is detected

## Building the Project

### Prerequisites
- Windows 10/11
- Visual Studio 2022 or Build Tools
- CMake 3.20+
- Windows SDK (for WASAPI headers)

### Build Commands
```bash
cmake -B build
cmake --build build --config Release
```

### Output
- **Executable**: `build/Release/meetnotes.exe`
- **Dependencies**: `build/bin/Release/*.dll` (whisper.cpp libraries)

## Usage

```bash
# Basic usage - records for 10 minutes by default
./meetnotes.exe

# Custom duration and output file
./meetnotes.exe --seconds 300 --out call.wav

# Specify whisper model
./meetnotes.exe --model models/ggml-base.en.bin --seconds 600
```

### Command Line Options
- `--model path`: Whisper model file (default: `models/ggml-small.en-tdrz.bin`)
- `--seconds N`: Recording duration in seconds (default: 600)
- `--out file`: Output WAV file (default: `meeting.wav`)
- `-h, --help`: Show help message

## Technical Advantages

### Performance Benefits
- **Lower latency**: Direct WASAPI access without wrapper libraries
- **Reduced overhead**: No format conversion through multiple layers
- **Event-driven**: CPU efficient, only processes when audio is available
- **MMCSS scheduling**: Real-time thread priority for audio processing

### Reliability Benefits
- **No external dependencies**: Fewer points of failure
- **Native integration**: Uses Windows audio engine directly
- **Buffer management**: Proper handling of audio engine timing
- **Error handling**: Comprehensive HRESULT checking and recovery

### Maintenance Benefits
- **Simpler build**: No external library management
- **Smaller footprint**: Reduced executable size and dependencies
- **Platform optimization**: Windows-specific optimizations
- **Direct debugging**: Native debugging without wrapper abstractions

## Audio Quality

The WASAPI loopback implementation captures:
- **Global audio mix**: All system audio including applications, notifications
- **Post-processing audio**: Includes system volume, effects, and session volumes
- **High fidelity**: Native sample rates (typically 44.1kHz or 48kHz)
- **Low noise floor**: Direct digital capture without analog conversion

## Future Enhancements

Potential areas for improvement:
1. **WinINet HTTP client**: For OpenAI API integration
2. **Device change notifications**: Automatic handling of audio device switches
3. **Process-specific capture**: Using Application Loopback Audio for app-specific recording
4. **Real-time VAD**: Voice activity detection during capture
5. **Format options**: Support for different output formats (FLAC, MP3)

## Troubleshooting

### Common Issues
1. **No audio captured**: Ensure some audio is playing during recording
2. **Access denied**: Run as administrator if needed
3. **Missing DLLs**: Copy whisper.cpp DLLs to executable directory
4. **Device not found**: Check default audio device settings

### Debug Tips
- Monitor console output for device enumeration
- Check Windows audio settings for default device
- Verify whisper model files exist in `models/` directory
- Test with simple audio playback during recording

## Conclusion

The migration to native WASAPI provides a robust, efficient, and maintainable solution for system audio capture on Windows. The implementation follows Microsoft's best practices for WASAPI loopback recording and provides high-quality audio capture suitable for speech recognition and transcription applications.
