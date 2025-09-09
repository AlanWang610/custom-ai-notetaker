# WASAPI Loopback Integration Guide

## 🎤 **What is WASAPI Loopback Recording?**

Windows Audio Session API (WASAPI) loopback mode allows your application to capture the exact audio that's being played through your system's speakers. This means you can record:

- **Meeting audio** from Zoom, Teams, Google Meet, etc.
- **System audio** from any application
- **Music, videos, or any sound** playing on your computer
- **Perfect quality** audio without acoustic feedback

## 🚀 **Usage**

### **Basic Usage:**
```powershell
# Record system audio for 5 minutes using WASAPI loopback
.\meetnotes.exe --loopback --seconds 300 --out meeting.wav

# Use a different Whisper model
.\meetnotes.exe --loopback --model models/ggml-base.en.bin --out call.wav
```

### **Command Line Options:**
```
--loopback              Use WASAPI loopback to capture system audio (Windows only)
--seconds N             Recording duration in seconds (default: 600)
--out wav              Output WAV file (default: meeting.wav)
--model path           Whisper model file (default: models/ggml-small.en-tdrz.bin)
```

## 🏗️ **Implementation Details**

### **Key Components Added:**

1. **WASAPILoopbackRecorder Class** (Windows only)
   - COM initialization and cleanup
   - Audio device enumeration
   - Loopback mode initialization
   - Thread-safe audio capture
   - Format conversion (int16/int32/float32 → float32)
   - Real-time buffer management

2. **Device Enumeration**
   - Lists all available render devices
   - Shows friendly device names
   - Automatic default device selection

3. **Audio Format Handling**
   - Automatic sample rate detection (typically 44.1kHz or 48kHz)
   - Multi-channel support (stereo → mono conversion for Whisper)
   - Real-time format conversion to WAV

### **Technical Features:**

- **Thread-safe recording** with atomic stop flags
- **High-priority audio thread** using `AvSetMmThreadCharacteristics`
- **Real-time format conversion** from various Windows audio formats
- **Continuous buffer writing** to prevent data loss
- **Silent period handling** (no data when no audio is playing)
- **Proper COM resource management**

## 🔧 **Build Dependencies**

Updated CMakeLists.txt includes Windows WASAPI libraries:
```cmake
# Windows-specific WASAPI libraries
if(WIN32)
  target_link_libraries(meetnotes PRIVATE
    ole32          # COM support
    oleaut32       # COM automation
    mmdevapi       # Multimedia device API
    audioclient    # Audio client interfaces
    avrt)          # Audio/video real-time support
endif()
```

## 📋 **Audio Flow**

1. **Initialize WASAPI**
   - Create device enumerator
   - Get default render device
   - Activate audio client
   - Get mix format

2. **Setup Loopback Capture**
   - Initialize client in `AUDCLNT_SHAREMODE_SHARED` mode
   - Set `AUDCLNT_STREAMFLAGS_LOOPBACK` flag
   - Get capture client interface

3. **Record Audio**
   - Start capture in high-priority thread
   - Continuously read audio packets
   - Convert format to float32
   - Write to buffer thread-safely

4. **Save to WAV**
   - Convert float32 to int16 for WAV format
   - Write to file using libsndfile
   - Maintain proper timing and sample rate

## ⚡ **Performance Optimizations**

- **Minimal buffer copying** with direct memory access
- **Lock-free atomic operations** for thread communication
- **High-priority thread** prevents audio dropouts
- **Efficient format conversion** with SIMD-friendly loops
- **Small sleep intervals** (1ms) to prevent CPU spinning

## 🎯 **Use Cases**

### **Perfect for:**
- **Recording online meetings** (Zoom, Teams, Meet)
- **Capturing webinars or presentations**
- **Recording system audio from any app**
- **Creating meeting transcripts**
- **Audio content analysis**

### **Advantages over Microphone Recording:**
- ✅ **Perfect audio quality** (no acoustic interference)
- ✅ **No background noise** or echo
- ✅ **Captures all participants** in meetings
- ✅ **System volume independent**
- ✅ **No feedback loops**

## 🔍 **Troubleshooting**

### **Common Issues:**

1. **No audio captured:**
   - Ensure audio is actually playing on the system
   - Check that the default playback device is active
   - Verify system volume is not muted

2. **COM initialization failed:**
   - Run as administrator if needed
   - Check Windows audio service is running

3. **Build errors:**
   - Ensure Windows SDK is installed
   - Check that WASAPI libraries are linked correctly

### **Debug Information:**
The application shows:
- Available audio render devices
- Selected audio format (sample rate, channels)
- Recording progress and status

## 🔄 **Fallback Behavior**

- **Windows**: Falls back to PortAudio if WASAPI initialization fails
- **Non-Windows**: Automatically uses PortAudio with a warning message
- **Cross-platform compatibility** maintained

## 📝 **Example Output**

```
Using WASAPI loopback mode to capture system audio output...
Available audio render devices:
  [0] Speakers (Realtek High Definition Audio)
  [1] Headphones (USB Audio Device)
Audio format: 48000 Hz, 2 channels
Starting WASAPI loopback recording for 300 seconds...
Recording system audio output to: meeting.wav
Make sure some audio is playing for best results!
WASAPI loopback recording completed!
```

## 🌟 **Benefits for Meeting Notes**

1. **Perfect Transcription Quality**: Clean audio = better Whisper results
2. **All Participants Captured**: Records everyone in the meeting clearly
3. **Zero Setup**: No microphone positioning or audio routing needed
4. **Reliable Recording**: No missed words due to poor microphone placement
5. **Professional Results**: Crystal clear audio for AI processing

This WASAPI loopback integration transforms your AI note taker into a professional-grade meeting recording and transcription system! 🎉
