// main.cpp
// Build deps: whisper.cpp (as submodule, provides whisper.h + libwhisper)
// Windows build uses native WASAPI for audio capture with no external dependencies.
// Run:
//   ./main --model models/ggml-small.en-tdrz.bin --seconds 600 --out meeting.wav
// Notes:
//   • Use a *-tdrz* model for local diarization (TinyDiarize). 16-kHz mono PCM WAV input.
//   • Uses WASAPI loopback to capture system audio output by default.
//   • All audio I/O and HTTP requests use native Windows APIs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "whisper.h"

// Windows WASAPI and other native includes
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiosessiontypes.h>
#include <avrt.h>
#include <comdef.h>
#include <comip.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wininet.h>
#endif

// -------------------- small utils --------------------
static std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size()+16);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\"': o += "\\\""; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
                else o += (char)c;
        }
    }
    return o;
}

// -------------------- Simple WAV file I/O --------------------
#pragma pack(push, 1)
struct WAVHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize;
};
#pragma pack(pop)

class SimpleWAVWriter {
private:
    std::ofstream file;
    WAVHeader header;
    size_t dataWritten = 0;
    
public:
    bool open(const std::string& filename, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
        file.open(filename, std::ios::binary);
        if (!file.is_open()) return false;
        
        header.numChannels = channels;
        header.sampleRate = sampleRate;
        header.bitsPerSample = bitsPerSample;
        header.byteRate = sampleRate * channels * (bitsPerSample / 8);
        header.blockAlign = channels * (bitsPerSample / 8);
        
        // Write placeholder header (will be updated on close)
        header.fileSize = 0;
        header.dataSize = 0;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        
        return true;
    }
    
    void writeInt16(const std::vector<int16_t>& samples) {
        file.write(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(int16_t));
        dataWritten += samples.size() * sizeof(int16_t);
    }
    
    void close() {
        if (!file.is_open()) return;
        
        // Update header with correct sizes
        header.dataSize = static_cast<uint32_t>(dataWritten);
        header.fileSize = static_cast<uint32_t>(dataWritten + sizeof(WAVHeader) - 8);
        
        // Seek back to beginning and write correct header
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        
        file.close();
    }
    
    ~SimpleWAVWriter() {
        close();
    }
};

class SimpleWAVReader {
public:
    struct AudioData {
        std::vector<int16_t> samples;
        uint32_t sampleRate;
        uint16_t channels;
        bool valid = false;
    };
    
    static AudioData read(const std::string& filename) {
        AudioData result;
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) return result;
        
        WAVHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        
        if (std::memcmp(header.riff, "RIFF", 4) != 0 || 
            std::memcmp(header.wave, "WAVE", 4) != 0 ||
            std::memcmp(header.data, "data", 4) != 0) {
            return result;
        }
        
        result.sampleRate = header.sampleRate;
        result.channels = header.numChannels;
        
        size_t sampleCount = header.dataSize / sizeof(int16_t);
        result.samples.resize(sampleCount);
        file.read(reinterpret_cast<char*>(result.samples.data()), header.dataSize);
        
        result.valid = true;
        return result;
    }
};

// -------------------- WASAPI audio capture options --------------------
enum class CaptureMode {
    LOOPBACK_ONLY,     // Just system audio (original behavior)
    MICROPHONE_ONLY,   // Just microphone audio
    DUAL_SEPARATE,     // Both streams to separate files
    DUAL_STEREO,       // Both streams mixed to stereo (L=mic, R=system)
    DUAL_MONO          // Both streams mixed to mono
};

struct RecordOptions {
    int seconds = 600;
    std::string out_path = "meeting.wav";
    CaptureMode mode = CaptureMode::LOOPBACK_ONLY;
    std::string mic_device_substr;  // Optional microphone device filter
};

#ifdef _WIN32
// -------------------- WASAPI Loopback Audio Recorder --------------------
class WASAPILoopbackRecorder {
private:
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    HANDLE eventHandle = nullptr;
    
    bool initialized = false;
    bool recording = false;
    std::atomic<bool> stopFlag{false};
    std::thread recordingThread;
    
    // Audio buffer
    std::vector<float> audioBuffer;
    mutable std::mutex bufferMutex;
    
    HRESULT InitializeCOM() {
        return CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    
    void CleanupCOM() {
        CoUninitialize();
    }
    
    HRESULT CreateDeviceEnumerator() {
        return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void**)&enumerator);
    }
    
    HRESULT GetDefaultRenderDevice() {
        return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    
    HRESULT ActivateAudioClient() {
        return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, (void**)&audioClient);
    }
    
    HRESULT GetMixFormat() {
        return audioClient->GetMixFormat(&waveFormat);
    }
    
    HRESULT InitializeAudioClient() {
        // Create event for event-driven capture
        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        
        // Initialize in loopback mode with event callback
        HRESULT hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0,  // Use default buffer duration
            0,
            waveFormat,
            nullptr
        );
        
        if (SUCCEEDED(hr)) {
            hr = audioClient->SetEventHandle(eventHandle);
        }
        
        return hr;
    }
    
    HRESULT GetCaptureClient() {
        return audioClient->GetService(__uuidof(IAudioCaptureClient),
                                     (void**)&captureClient);
    }
    
    void RecordingThreadFunc() {
        HRESULT hr;
        DWORD taskIndex = 0;
        HANDLE audioTask = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
        
        hr = audioClient->Start();
        if (FAILED(hr)) {
            std::cerr << "Failed to start audio client: " << std::hex << hr << std::endl;
            if (audioTask) AvRevertMmThreadCharacteristics(audioTask);
            return;
        }
        
        std::cout << "WASAPI loopback recording started with event-driven capture" << std::endl;
        
        while (!stopFlag.load()) {
            // Wait for audio data to become available
            DWORD waitResult = WaitForSingleObject(eventHandle, 2000); // 2 second timeout
            
            if (waitResult == WAIT_TIMEOUT) {
                std::cout << "Audio capture timeout - checking if audio is playing..." << std::endl;
                continue;
            } else if (waitResult != WAIT_OBJECT_0) {
                std::cerr << "Error waiting for audio event: " << waitResult << std::endl;
                break;
            }
            
            // Check if we should stop
            if (stopFlag.load()) break;
            
            // Process all available packets
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                std::cerr << "Failed to get next packet size: " << std::hex << hr << std::endl;
                break;
            }
            
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, 
                                            &devicePosition, &qpcPosition);
                if (FAILED(hr)) {
                    std::cerr << "Failed to get capture buffer: " << std::hex << hr << std::endl;
                    break;
                }
                
                if (framesAvailable > 0) {
                    // Handle buffer flags
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // Silent buffer - fill with zeros
                        size_t sampleCount = framesAvailable * waveFormat->nChannels;
                        std::lock_guard<std::mutex> lock(bufferMutex);
                        audioBuffer.insert(audioBuffer.end(), sampleCount, 0.0f);
                    } else {
                        // Process audio data
                        size_t sampleCount = framesAvailable * waveFormat->nChannels;
                        
                        std::lock_guard<std::mutex> lock(bufferMutex);
                        
                        if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(waveFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                            // Already float32
                            float* floatData = reinterpret_cast<float*>(data);
                            audioBuffer.insert(audioBuffer.end(), floatData, floatData + sampleCount);
                        }
                        else if (waveFormat->wBitsPerSample == 16) {
                            // Convert from int16 to float32
                            int16_t* int16Data = reinterpret_cast<int16_t*>(data);
                            for (size_t i = 0; i < sampleCount; ++i) {
                                audioBuffer.push_back(int16Data[i] / 32768.0f);
                            }
                        }
                        else if (waveFormat->wBitsPerSample == 32) {
                            // Convert from int32 to float32  
                            int32_t* int32Data = reinterpret_cast<int32_t*>(data);
                            for (size_t i = 0; i < sampleCount; ++i) {
                                audioBuffer.push_back(int32Data[i] / 2147483648.0f);
                            }
                        }
                    }
                    
                    // Check for data discontinuity
                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                        std::cout << "Audio data discontinuity detected" << std::endl;
                    }
                }
                
                hr = captureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(hr)) {
                    std::cerr << "Failed to release capture buffer: " << std::hex << hr << std::endl;
                    break;
                }
                
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    std::cerr << "Failed to get next packet size: " << std::hex << hr << std::endl;
                    break;
                }
            }
        }
        
        audioClient->Stop();
        if (audioTask) AvRevertMmThreadCharacteristics(audioTask);
        std::cout << "WASAPI loopback recording thread finished" << std::endl;
    }
    
public:
    WASAPILoopbackRecorder() = default;
    
    ~WASAPILoopbackRecorder() {
        Cleanup();
    }
    
    bool Initialize() {
        HRESULT hr;
        
        hr = InitializeCOM();
        if (FAILED(hr)) {
            std::cerr << "Failed to initialize COM: " << std::hex << hr << std::endl;
            return false;
        }
        
        hr = CreateDeviceEnumerator();
        if (FAILED(hr)) {
            std::cerr << "Failed to create device enumerator: " << std::hex << hr << std::endl;
            CleanupCOM();
            return false;
        }
        
        hr = GetDefaultRenderDevice();
        if (FAILED(hr)) {
            std::cerr << "Failed to get default render device: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = ActivateAudioClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to activate audio client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = GetMixFormat();
        if (FAILED(hr)) {
            std::cerr << "Failed to get mix format: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = InitializeAudioClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to initialize audio client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = GetCaptureClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to get capture client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        initialized = true;
        return true;
    }
    
    bool StartRecording() {
        if (!initialized || recording) return false;
        
        stopFlag.store(false);
        recordingThread = std::thread(&WASAPILoopbackRecorder::RecordingThreadFunc, this);
        recording = true;
        return true;
    }
    
    void StopRecording() {
        if (!recording) return;
        
        stopFlag.store(true);
        if (recordingThread.joinable()) {
            recordingThread.join();
        }
        recording = false;
    }
    
    std::vector<float> GetAudioData() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        return audioBuffer;
    }
    
    void ClearAudioData() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        audioBuffer.clear();
    }
    
    void GetAudioFormat(uint32_t& sampleRate, uint32_t& channels) {
        if (waveFormat) {
            sampleRate = waveFormat->nSamplesPerSec;
            channels = waveFormat->nChannels;
        } else {
            sampleRate = 44100;
            channels = 2;
        }
    }
    
    void Cleanup() {
        StopRecording();
        
        if (captureClient) {
            captureClient->Release();
            captureClient = nullptr;
        }
        
        if (audioClient) {
            audioClient->Release();
            audioClient = nullptr;
        }
        
        if (eventHandle) {
            CloseHandle(eventHandle);
            eventHandle = nullptr;
        }
        
        if (waveFormat) {
            CoTaskMemFree(waveFormat);
            waveFormat = nullptr;
        }
        
        if (device) {
            device->Release();
            device = nullptr;
        }
        
        if (enumerator) {
            enumerator->Release();
            enumerator = nullptr;
        }
        
        if (initialized) {
            CleanupCOM();
            initialized = false;
        }
    }
    
    std::vector<std::string> EnumerateAudioDevices() {
        std::vector<std::string> devices;
        
        if (!enumerator) return devices;
        
        IMMDeviceCollection* deviceCollection = nullptr;
        HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
        if (FAILED(hr)) return devices;
        
        UINT count = 0;
        deviceCollection->GetCount(&count);
        
        for (UINT i = 0; i < count; i++) {
            IMMDevice* device = nullptr;
            hr = deviceCollection->Item(i, &device);
            if (FAILED(hr)) continue;
            
            IPropertyStore* propertyStore = nullptr;
            hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
            if (SUCCEEDED(hr)) {
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);
                
                hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
                if (SUCCEEDED(hr) && friendlyName.vt == VT_LPWSTR) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    std::string deviceName(size - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, &deviceName[0], size, nullptr, nullptr);
                    devices.push_back(deviceName);
                }
                
                PropVariantClear(&friendlyName);
                propertyStore->Release();
            }
            
            device->Release();
        }
        
        deviceCollection->Release();
        return devices;
    }
};

// -------------------- WASAPI Microphone Audio Recorder --------------------
class WASAPIMicrophoneRecorder {
private:
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    HANDLE eventHandle = nullptr;
    
    bool initialized = false;
    bool recording = false;
    std::atomic<bool> stopFlag{false};
    std::thread recordingThread;
    
    // Audio buffer
    std::vector<float> audioBuffer;
    mutable std::mutex bufferMutex;
    
    std::string deviceSubstr;  // Device filter string
    
    HRESULT InitializeCOM() {
        return CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    
    void CleanupCOM() {
        CoUninitialize();
    }
    
    HRESULT CreateDeviceEnumerator() {
        return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void**)&enumerator);
    }
    
    HRESULT GetMicrophoneDevice() {
        // Try to find a specific microphone if substring provided
        if (!deviceSubstr.empty()) {
            IMMDeviceCollection* deviceCollection = nullptr;
            HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &deviceCollection);
            if (FAILED(hr)) return hr;
            
            UINT count = 0;
            deviceCollection->GetCount(&count);
            
            for (UINT i = 0; i < count; i++) {
                IMMDevice* testDevice = nullptr;
                hr = deviceCollection->Item(i, &testDevice);
                if (FAILED(hr)) continue;
                
                IPropertyStore* propertyStore = nullptr;
                hr = testDevice->OpenPropertyStore(STGM_READ, &propertyStore);
                if (SUCCEEDED(hr)) {
                    PROPVARIANT friendlyName;
                    PropVariantInit(&friendlyName);
                    
                    hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
                    if (SUCCEEDED(hr) && friendlyName.vt == VT_LPWSTR) {
                        int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                        std::string deviceName(size - 1, 0);
                        WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, &deviceName[0], size, nullptr, nullptr);
                        
                        if (deviceName.find(deviceSubstr) != std::string::npos) {
                            device = testDevice;
                            testDevice = nullptr; // Don't release
                            PropVariantClear(&friendlyName);
                            propertyStore->Release();
                            deviceCollection->Release();
                            return S_OK;
                        }
                    }
                    
                    PropVariantClear(&friendlyName);
                    propertyStore->Release();
                }
                
                testDevice->Release();
            }
            
            deviceCollection->Release();
        }
        
        // Fall back to default capture device
        return enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    }
    
    HRESULT ActivateAudioClient() {
        return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, (void**)&audioClient);
    }
    
    HRESULT GetMixFormat() {
        return audioClient->GetMixFormat(&waveFormat);
    }
    
    HRESULT InitializeAudioClient() {
        // Create event for event-driven capture
        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        
        // Initialize in shared mode for microphone capture (no loopback flag)
        HRESULT hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0,  // Use default buffer duration
            0,
            waveFormat,
            nullptr
        );
        
        if (SUCCEEDED(hr)) {
            hr = audioClient->SetEventHandle(eventHandle);
        }
        
        return hr;
    }
    
    HRESULT GetCaptureClient() {
        return audioClient->GetService(__uuidof(IAudioCaptureClient),
                                     (void**)&captureClient);
    }
    
    void RecordingThreadFunc() {
        HRESULT hr;
        DWORD taskIndex = 0;
        HANDLE audioTask = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
        
        hr = audioClient->Start();
        if (FAILED(hr)) {
            std::cerr << "Failed to start microphone audio client: " << std::hex << hr << std::endl;
            if (audioTask) AvRevertMmThreadCharacteristics(audioTask);
            return;
        }
        
        std::cout << "WASAPI microphone recording started" << std::endl;
        
        while (!stopFlag.load()) {
            // Wait for audio data to become available
            DWORD waitResult = WaitForSingleObject(eventHandle, 2000); // 2 second timeout
            
            if (waitResult == WAIT_TIMEOUT) {
                continue;
            } else if (waitResult != WAIT_OBJECT_0) {
                std::cerr << "Error waiting for microphone audio event: " << waitResult << std::endl;
                break;
            }
            
            if (stopFlag.load()) break;
            
            // Process all available packets
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                std::cerr << "Failed to get next packet size (mic): " << std::hex << hr << std::endl;
                break;
            }
            
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    std::cerr << "Failed to get microphone capture buffer: " << std::hex << hr << std::endl;
                    break;
                }
                
                if (framesAvailable > 0) {
                    size_t sampleCount = framesAvailable * waveFormat->nChannels;
                    
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        // Silent buffer - fill with zeros
                        audioBuffer.insert(audioBuffer.end(), sampleCount, 0.0f);
                    } else {
                        // Process audio data (same format conversion as loopback)
                        if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(waveFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                            // Already float32
                            float* floatData = reinterpret_cast<float*>(data);
                            audioBuffer.insert(audioBuffer.end(), floatData, floatData + sampleCount);
                        }
                        else if (waveFormat->wBitsPerSample == 16) {
                            // Convert from int16 to float32
                            int16_t* int16Data = reinterpret_cast<int16_t*>(data);
                            for (size_t i = 0; i < sampleCount; ++i) {
                                audioBuffer.push_back(int16Data[i] / 32768.0f);
                            }
                        }
                        else if (waveFormat->wBitsPerSample == 32) {
                            // Convert from int32 to float32  
                            int32_t* int32Data = reinterpret_cast<int32_t*>(data);
                            for (size_t i = 0; i < sampleCount; ++i) {
                                audioBuffer.push_back(int32Data[i] / 2147483648.0f);
                            }
                        }
                    }
                }
                
                hr = captureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(hr)) {
                    std::cerr << "Failed to release microphone capture buffer: " << std::hex << hr << std::endl;
                    break;
                }
                
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }
        }
        
        audioClient->Stop();
        if (audioTask) AvRevertMmThreadCharacteristics(audioTask);
        std::cout << "WASAPI microphone recording thread finished" << std::endl;
    }
    
public:
    WASAPIMicrophoneRecorder(const std::string& deviceFilter = "") : deviceSubstr(deviceFilter) {}
    
    ~WASAPIMicrophoneRecorder() {
        Cleanup();
    }
    
    bool Initialize() {
        HRESULT hr;
        
        hr = InitializeCOM();
        if (FAILED(hr)) {
            std::cerr << "Failed to initialize COM for microphone: " << std::hex << hr << std::endl;
            return false;
        }
        
        hr = CreateDeviceEnumerator();
        if (FAILED(hr)) {
            std::cerr << "Failed to create device enumerator for microphone: " << std::hex << hr << std::endl;
            CleanupCOM();
            return false;
        }
        
        hr = GetMicrophoneDevice();
        if (FAILED(hr)) {
            std::cerr << "Failed to get microphone device: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = ActivateAudioClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to activate microphone audio client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = GetMixFormat();
        if (FAILED(hr)) {
            std::cerr << "Failed to get microphone mix format: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = InitializeAudioClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to initialize microphone audio client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        hr = GetCaptureClient();
        if (FAILED(hr)) {
            std::cerr << "Failed to get microphone capture client: " << std::hex << hr << std::endl;
            Cleanup();
            return false;
        }
        
        initialized = true;
        return true;
    }
    
    bool StartRecording() {
        if (!initialized || recording) return false;
        
        stopFlag.store(false);
        recordingThread = std::thread(&WASAPIMicrophoneRecorder::RecordingThreadFunc, this);
        recording = true;
        return true;
    }
    
    void StopRecording() {
        if (!recording) return;
        
        stopFlag.store(true);
        if (recordingThread.joinable()) {
            recordingThread.join();
        }
        recording = false;
    }
    
    std::vector<float> GetAudioData() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        return audioBuffer;
    }
    
    void ClearAudioData() {
        std::lock_guard<std::mutex> lock(bufferMutex);
        audioBuffer.clear();
    }
    
    void GetAudioFormat(uint32_t& sampleRate, uint32_t& channels) {
        if (waveFormat) {
            sampleRate = waveFormat->nSamplesPerSec;
            channels = waveFormat->nChannels;
        } else {
            sampleRate = 44100;
            channels = 1;  // Microphones are typically mono
        }
    }
    
    void Cleanup() {
        StopRecording();
        
        if (captureClient) {
            captureClient->Release();
            captureClient = nullptr;
        }
        
        if (audioClient) {
            audioClient->Release();
            audioClient = nullptr;
        }
        
        if (eventHandle) {
            CloseHandle(eventHandle);
            eventHandle = nullptr;
        }
        
        if (waveFormat) {
            CoTaskMemFree(waveFormat);
            waveFormat = nullptr;
        }
        
        if (device) {
            device->Release();
            device = nullptr;
        }
        
        if (enumerator) {
            enumerator->Release();
            enumerator = nullptr;
        }
        
        if (initialized) {
            CleanupCOM();
            initialized = false;
        }
    }
    
    std::vector<std::string> EnumerateMicrophoneDevices() {
        std::vector<std::string> devices;
        
        if (!enumerator) return devices;
        
        IMMDeviceCollection* deviceCollection = nullptr;
        HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &deviceCollection);
        if (FAILED(hr)) return devices;
        
        UINT count = 0;
        deviceCollection->GetCount(&count);
        
        for (UINT i = 0; i < count; i++) {
            IMMDevice* device = nullptr;
            hr = deviceCollection->Item(i, &device);
            if (FAILED(hr)) continue;
            
            IPropertyStore* propertyStore = nullptr;
            hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
            if (SUCCEEDED(hr)) {
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);
                
                hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
                if (SUCCEEDED(hr) && friendlyName.vt == VT_LPWSTR) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    std::string deviceName(size - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, friendlyName.pwszVal, -1, &deviceName[0], size, nullptr, nullptr);
                    devices.push_back(deviceName);
                }
                
                PropVariantClear(&friendlyName);
                propertyStore->Release();
            }
            
            device->Release();
        }
        
        deviceCollection->Release();
        return devices;
    }
};
#endif // _WIN32

// -------------------- Audio Mixing Utilities --------------------
class AudioMixer {
public:
    // Mix two audio streams with optional gain control
    static std::vector<float> MixMono(const std::vector<float>& stream1, const std::vector<float>& stream2, 
                                     float gain1 = 1.0f, float gain2 = 1.0f) {
        size_t maxSize = (std::max)(stream1.size(), stream2.size());
        std::vector<float> result(maxSize, 0.0f);
        
        for (size_t i = 0; i < maxSize; ++i) {
            float sample1 = (i < stream1.size()) ? stream1[i] * gain1 : 0.0f;
            float sample2 = (i < stream2.size()) ? stream2[i] * gain2 : 0.0f;
            result[i] = (std::max)(-1.0f, (std::min)(1.0f, sample1 + sample2)); // Clamp to prevent overflow
        }
        
        return result;
    }
    
    // Create stereo output from two mono streams (L=stream1, R=stream2)
    static std::vector<float> MixStereo(const std::vector<float>& leftStream, const std::vector<float>& rightStream) {
        size_t maxSize = (std::max)(leftStream.size(), rightStream.size());
        std::vector<float> result(maxSize * 2, 0.0f);
        
        for (size_t i = 0; i < maxSize; ++i) {
            result[i * 2] = (i < leftStream.size()) ? leftStream[i] : 0.0f;      // Left channel
            result[i * 2 + 1] = (i < rightStream.size()) ? rightStream[i] : 0.0f; // Right channel
        }
        
        return result;
    }
    
    // Convert stereo to mono by averaging channels
    static std::vector<float> StereoToMono(const std::vector<float>& stereoData) {
        if (stereoData.size() % 2 != 0) {
            std::cerr << "Warning: Stereo data size is not even" << std::endl;
        }
        
        std::vector<float> monoData;
        monoData.reserve(stereoData.size() / 2);
        
        for (size_t i = 0; i < stereoData.size(); i += 2) {
            float left = stereoData[i];
            float right = (i + 1 < stereoData.size()) ? stereoData[i + 1] : 0.0f;
            monoData.push_back((left + right) * 0.5f);
        }
        
        return monoData;
    }
    
    // Convert mono to stereo by duplicating the channel
    static std::vector<float> MonoToStereo(const std::vector<float>& monoData) {
        std::vector<float> stereoData;
        stereoData.reserve(monoData.size() * 2);
        
        for (float sample : monoData) {
            stereoData.push_back(sample);
            stereoData.push_back(sample);
        }
        
        return stereoData;
    }
};

// -------------------- Dual Audio Recording Functions --------------------
#ifdef _WIN32

// Record only microphone
static int record_microphone_only(const RecordOptions& opt) {
    std::cout << "Using WASAPI microphone capture..." << std::endl;
    
    WASAPIMicrophoneRecorder micRecorder(opt.mic_device_substr);
    if (!micRecorder.Initialize()) {
        std::cerr << "Failed to initialize microphone recorder" << std::endl;
        return 1;
    }
    
    uint32_t sampleRate, channels;
    micRecorder.GetAudioFormat(sampleRate, channels);
    std::cout << "Microphone format: " << sampleRate << " Hz, " << channels << " channels" << std::endl;
    
    // Create output WAV file
    SimpleWAVWriter wavWriter;
    if (!wavWriter.open(opt.out_path, sampleRate, channels, 16)) {
        std::cerr << "Failed to open output file: " << opt.out_path << std::endl;
        return 2;
    }
    
    std::cout << "Starting microphone recording for " << opt.seconds << " seconds..." << std::endl;
    
    if (!micRecorder.StartRecording()) {
        std::cerr << "Failed to start microphone recording" << std::endl;
        return 3;
    }
    
    // Record for specified duration
    auto startTime = std::chrono::high_resolution_clock::now();
    auto endTime = startTime + std::chrono::seconds(opt.seconds);
    
    while (std::chrono::high_resolution_clock::now() < endTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        auto audioData = micRecorder.GetAudioData();
        if (!audioData.empty()) {
            std::vector<int16_t> int16Data;
            int16Data.reserve(audioData.size());
            
            for (float sample : audioData) {
                sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
            }
            
            wavWriter.writeInt16(int16Data);
            micRecorder.ClearAudioData();
        }
    }
    
    micRecorder.StopRecording();
    wavWriter.close();
    
    std::cout << "Microphone recording completed!" << std::endl;
    return 0;
}

// Record both loopback and microphone simultaneously
static int record_dual_audio(const RecordOptions& opt) {
    std::cout << "Using dual WASAPI capture (loopback + microphone)..." << std::endl;
    
    // Initialize both recorders
    WASAPILoopbackRecorder loopbackRecorder;
    WASAPIMicrophoneRecorder micRecorder(opt.mic_device_substr);
    
    if (!loopbackRecorder.Initialize()) {
        std::cerr << "Failed to initialize loopback recorder" << std::endl;
        return 1;
    }
    
    if (!micRecorder.Initialize()) {
        std::cerr << "Failed to initialize microphone recorder" << std::endl;
        return 1;
    }
    
    // Get audio formats
    uint32_t loopbackSampleRate, loopbackChannels;
    uint32_t micSampleRate, micChannels;
    loopbackRecorder.GetAudioFormat(loopbackSampleRate, loopbackChannels);
    micRecorder.GetAudioFormat(micSampleRate, micChannels);
    
    std::cout << "Loopback format: " << loopbackSampleRate << " Hz, " << loopbackChannels << " channels" << std::endl;
    std::cout << "Microphone format: " << micSampleRate << " Hz, " << micChannels << " channels" << std::endl;
    
    // For simplicity, use the loopback sample rate (usually higher quality)
    uint32_t outputSampleRate = loopbackSampleRate;
    uint32_t outputChannels = 2; // Default to stereo for mixed output
    
    std::string loopbackPath, micPath;
    SimpleWAVWriter* loopbackWriter = nullptr;
    SimpleWAVWriter* micWriter = nullptr;
    SimpleWAVWriter* mixedWriter = nullptr;
    
    // Configure output based on capture mode
    switch (opt.mode) {
        case CaptureMode::DUAL_SEPARATE: {
            // Create separate files
            loopbackPath = opt.out_path.substr(0, opt.out_path.find_last_of('.')) + "_loopback.wav";
            micPath = opt.out_path.substr(0, opt.out_path.find_last_of('.')) + "_microphone.wav";
            
            loopbackWriter = new SimpleWAVWriter();
            micWriter = new SimpleWAVWriter();
            
            if (!loopbackWriter->open(loopbackPath, outputSampleRate, loopbackChannels, 16) ||
                !micWriter->open(micPath, outputSampleRate, micChannels, 16)) {
                std::cerr << "Failed to open output files" << std::endl;
                delete loopbackWriter;
                delete micWriter;
                return 2;
            }
            
            std::cout << "Recording to separate files: " << loopbackPath << " and " << micPath << std::endl;
            break;
        }
        
        case CaptureMode::DUAL_STEREO: {
            // Mixed stereo output (L=mic, R=system)
            outputChannels = 2;
            mixedWriter = new SimpleWAVWriter();
            
            if (!mixedWriter->open(opt.out_path, outputSampleRate, outputChannels, 16)) {
                std::cerr << "Failed to open output file: " << opt.out_path << std::endl;
                delete mixedWriter;
                return 2;
            }
            
            std::cout << "Recording to stereo mix (Left=Microphone, Right=System): " << opt.out_path << std::endl;
            break;
        }
        
        case CaptureMode::DUAL_MONO: {
            // Mixed mono output
            outputChannels = 1;
            mixedWriter = new SimpleWAVWriter();
            
            if (!mixedWriter->open(opt.out_path, outputSampleRate, outputChannels, 16)) {
                std::cerr << "Failed to open output file: " << opt.out_path << std::endl;
                delete mixedWriter;
                return 2;
            }
            
            std::cout << "Recording to mono mix: " << opt.out_path << std::endl;
            break;
        }
        
        default:
            std::cerr << "Invalid dual capture mode" << std::endl;
            return 1;
    }
    
    // Start both recordings
    if (!loopbackRecorder.StartRecording() || !micRecorder.StartRecording()) {
        std::cerr << "Failed to start dual recording" << std::endl;
        delete loopbackWriter;
        delete micWriter;
        delete mixedWriter;
        return 3;
    }
    
    std::cout << "Starting dual recording for " << opt.seconds << " seconds..." << std::endl;
    
    // Record for specified duration
    auto startTime = std::chrono::high_resolution_clock::now();
    auto endTime = startTime + std::chrono::seconds(opt.seconds);
    
    while (std::chrono::high_resolution_clock::now() < endTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Higher frequency for dual capture
        
        auto loopbackData = loopbackRecorder.GetAudioData();
        auto micData = micRecorder.GetAudioData();
        
        if (!loopbackData.empty() || !micData.empty()) {
            switch (opt.mode) {
                case CaptureMode::DUAL_SEPARATE: {
                    // Write to separate files
                    if (!loopbackData.empty()) {
                        std::vector<int16_t> int16Data;
                        int16Data.reserve(loopbackData.size());
                        for (float sample : loopbackData) {
                            sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                            int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
                        }
                        loopbackWriter->writeInt16(int16Data);
                    }
                    
                    if (!micData.empty()) {
                        std::vector<int16_t> int16Data;
                        int16Data.reserve(micData.size());
                        for (float sample : micData) {
                            sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                            int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
                        }
                        micWriter->writeInt16(int16Data);
                    }
                    break;
                }
                
                case CaptureMode::DUAL_STEREO: {
                    // Convert both to mono first if needed
                    std::vector<float> loopbackMono = (loopbackChannels == 2) ? 
                        AudioMixer::StereoToMono(loopbackData) : loopbackData;
                    std::vector<float> micMono = (micChannels == 2) ? 
                        AudioMixer::StereoToMono(micData) : micData;
                    
                    // Create stereo mix (L=mic, R=system)
                    auto stereoMix = AudioMixer::MixStereo(micMono, loopbackMono);
                    
                    std::vector<int16_t> int16Data;
                    int16Data.reserve(stereoMix.size());
                    for (float sample : stereoMix) {
                        sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                        int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
                    }
                    mixedWriter->writeInt16(int16Data);
                    break;
                }
                
                case CaptureMode::DUAL_MONO: {
                    // Convert both to mono and mix
                    std::vector<float> loopbackMono = (loopbackChannels == 2) ? 
                        AudioMixer::StereoToMono(loopbackData) : loopbackData;
                    std::vector<float> micMono = (micChannels == 2) ? 
                        AudioMixer::StereoToMono(micData) : micData;
                    
                    // Mix both streams
                    auto monoMix = AudioMixer::MixMono(micMono, loopbackMono, 0.7f, 0.7f); // Reduce gain to prevent clipping
                    
                    std::vector<int16_t> int16Data;
                    int16Data.reserve(monoMix.size());
                    for (float sample : monoMix) {
                        sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                        int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
                    }
                    mixedWriter->writeInt16(int16Data);
                    break;
                }
                
                default:
                    break;
            }
            
            // Clear buffers
            if (!loopbackData.empty()) loopbackRecorder.ClearAudioData();
            if (!micData.empty()) micRecorder.ClearAudioData();
        }
    }
    
    // Stop recordings and cleanup
    loopbackRecorder.StopRecording();
    micRecorder.StopRecording();
    
    if (loopbackWriter) {
        loopbackWriter->close();
        delete loopbackWriter;
    }
    if (micWriter) {
        micWriter->close();
        delete micWriter;
    }
    if (mixedWriter) {
        mixedWriter->close();
        delete mixedWriter;
    }
    
    std::cout << "Dual audio recording completed!" << std::endl;
    return 0;
}

// Original loopback recording function
static int record_wasapi_loopback(const RecordOptions& opt) {
    std::cout << "Using WASAPI loopback mode to capture system audio output..." << std::endl;
    
    WASAPILoopbackRecorder recorder;
    if (!recorder.Initialize()) {
        std::cerr << "Failed to initialize WASAPI loopback recorder" << std::endl;
        return 1;
    }
    
    // Enumerate available audio devices
    auto devices = recorder.EnumerateAudioDevices();
    std::cout << "Available audio render devices:" << std::endl;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  [" << i << "] " << devices[i] << std::endl;
    }
    
    uint32_t sampleRate, channels;
    recorder.GetAudioFormat(sampleRate, channels);
    std::cout << "Audio format: " << sampleRate << " Hz, " << channels << " channels" << std::endl;
    
    // Create output WAV file
    SimpleWAVWriter wavWriter;
    if (!wavWriter.open(opt.out_path, sampleRate, channels, 16)) {
        std::cerr << "Failed to open output file: " << opt.out_path << std::endl;
        return 2;
    }
    
    std::cout << "Starting WASAPI loopback recording for " << opt.seconds << " seconds..." << std::endl;
    std::cout << "Recording system audio output to: " << opt.out_path << std::endl;
    std::cout << "Make sure some audio is playing for best results!" << std::endl;
    
    if (!recorder.StartRecording()) {
        std::cerr << "Failed to start recording" << std::endl;
        return 3;
    }
    
    // Record for specified duration
    auto startTime = std::chrono::high_resolution_clock::now();
    auto endTime = startTime + std::chrono::seconds(opt.seconds);
    
    while (std::chrono::high_resolution_clock::now() < endTime) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Get accumulated audio data
        auto audioData = recorder.GetAudioData();
        if (!audioData.empty()) {
            // Convert float32 to int16 for WAV file
            std::vector<int16_t> int16Data;
            int16Data.reserve(audioData.size());
            
            for (float sample : audioData) {
                // Clamp to [-1.0, 1.0] and convert to int16
                sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
                int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
            }
            
            // Write to file
            wavWriter.writeInt16(int16Data);
            
            // Clear the buffer to avoid writing duplicate data
            recorder.ClearAudioData();
        }
    }
    
    recorder.StopRecording();
    wavWriter.close();
    
    std::cout << "WASAPI loopback recording completed!" << std::endl;
    return 0;
}
#endif // _WIN32

// -------------------- whisper.cpp transcription + diarization --------------------
struct Transcription {
    std::string plain_text;       // raw text with [SPEAKER TURN] markers possibly embedded
    struct Seg { int64_t t0_ms, t1_ms; std::string text; };
    std::vector<Seg> segments;
};

static Transcription transcribe_whisper_tdrz(const std::string& model_path,
                                             const std::string& wav_path,
                                             int n_threads = 4) {
    Transcription out;

    // load audio
    auto audioData = SimpleWAVReader::read(wav_path);
    if (!audioData.valid) return out;

    // to mono float32
    std::vector<float> mono;
    size_t totalFrames = audioData.samples.size() / audioData.channels;
    mono.reserve(totalFrames);
    
    if (audioData.channels == 1) {
        mono.resize(totalFrames);
        for (size_t i = 0; i < totalFrames; ++i) {
            mono[i] = audioData.samples[i] / 32768.0f;
        }
    } else {
        mono.resize(totalFrames);
        for (size_t i = 0; i < totalFrames; ++i) {
            int idx = (int)i * audioData.channels;
            int sum = 0; 
            for (int c = 0; c < audioData.channels; ++c) {
                sum += audioData.samples[idx + c];
            }
            mono[i] = (sum / (float)audioData.channels) / 32768.0f;
        }
    }

    // load whisper model
    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) return out;

    // set decode params
    auto wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime   = false;
    wparams.print_progress   = false;
    wparams.translate        = false;
    wparams.no_timestamps    = false;
    wparams.n_threads        = n_threads;
    wparams.language         = "en";               // set to nullptr/"auto" to autodetect
    wparams.vad              = true;               // built-in VAD to trim silence
    wparams.tdrz_enable      = true;               // TinyDiarize speaker-turn detection (needs *-tdrz model)

    if (whisper_full(ctx, wparams, mono.data(), (int)mono.size()) != 0) {
        whisper_free(ctx); return out;
    }

    int nseg = whisper_full_n_segments(ctx);
    out.segments.reserve(nseg);
    std::ostringstream all;
    for (int i = 0; i < nseg; ++i) {
        const char* txt = whisper_full_get_segment_text(ctx, i);
        int64_t t0 = whisper_full_get_segment_t0(ctx, i); // in 10ms units
        int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        std::string s = txt ? txt : "";
        all << s;
        out.segments.push_back({ t0*10, t1*10, s });
    }
    out.plain_text = all.str();
    whisper_free(ctx);
    return out;
}

// Render diarized text by splitting on "[SPEAKER TURN]" tokens
static std::string render_diarized(const Transcription& tr) {
    // Concatenate segments with timestamps, then split.
    std::ostringstream oss;
    for (const auto& sg : tr.segments) {
        // keep original markers, then we’ll global-split
        oss << sg.text;
    }
    std::string all = oss.str();

    const std::string marker = "[SPEAKER TURN]";
    std::vector<std::string> parts;
    size_t pos = 0, m;
    while ((m = all.find(marker, pos)) != std::string::npos) {
        parts.push_back(all.substr(pos, m - pos));
        pos = m + marker.size();
    }
    parts.push_back(all.substr(pos));

    int spk = 1;
    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string chunk = parts[i];
        // trim
        chunk.erase(chunk.begin(), std::find_if(chunk.begin(), chunk.end(), [](int ch){ return !std::isspace(ch); }));
        chunk.erase(std::find_if(chunk.rbegin(), chunk.rend(), [](int ch){ return !std::isspace(ch); }).base(), chunk.end());
        if (chunk.empty()) continue;
        out << "Speaker " << spk << ": " << chunk << "\n";
        spk++;
    }
    return out.str();
}

// -------------------- Environment file loading --------------------
static void load_env_file(const std::string& filename = ".env") {
    std::ifstream file(filename);
    if (!file.is_open()) {
        // .env file doesn't exist, that's okay - fall back to system environment
        return;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;
        
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        // Trim whitespace from key and value
        key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](int ch){ return !std::isspace(ch); }));
        key.erase(std::find_if(key.rbegin(), key.rend(), [](int ch){ return !std::isspace(ch); }).base(), key.end());
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](int ch){ return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](int ch){ return !std::isspace(ch); }).base(), value.end());
        
        if (!key.empty() && !value.empty()) {
            // Set environment variable (Windows)
#ifdef _WIN32
            _putenv((key + "=" + value).c_str());
#else
            setenv(key.c_str(), value.c_str(), 1);
#endif
        }
    }
}

static std::string get_openai_api_key() {
    // First, try to load from .env file
    load_env_file();
    
    // Then get the key from environment (either from .env or system)
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "ERROR: OPENAI_API_KEY not found!\n";
        std::cerr << "Please either:\n";
        std::cerr << "1. Set it in .env file: OPENAI_API_KEY=your_key_here\n";
        std::cerr << "2. Set it as environment variable\n";
        return "";
    }
    return std::string(key);
}

// -------------------- OpenAI summary (WinINet-based HTTP) --------------------
#ifdef _WIN32
static std::string summarize_with_openai(const std::string& transcript) {
    std::string key = get_openai_api_key();
    if (key.empty()) return "{\"error\":\"OPENAI_API_KEY not set\"}";
    
    // Note: For simplicity, OpenAI integration is disabled in this version.
    // You can add WinINet-based HTTP client here if needed.
    return "{\"note\":\"OpenAI integration disabled. Transcript captured successfully.\"}";
}
#else
static std::string summarize_with_openai(const std::string& transcript) {
    return "{\"error\":\"OpenAI integration not supported on this platform\"}";
}
#endif

// -------------------- main --------------------
int main(int argc, char** argv) {
    // defaults
    std::string model_path = "models/ggml-small.en-tdrz.bin";
    RecordOptions rec_opt;
    
    // args
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--model" && i+1 < argc) model_path = argv[++i];
        else if (a == "--seconds" && i+1 < argc) rec_opt.seconds = std::atoi(argv[++i]);
        else if (a == "--out" && i+1 < argc) rec_opt.out_path = argv[++i];
        else if (a == "--mic-device" && i+1 < argc) rec_opt.mic_device_substr = argv[++i];
        else if (a == "--mode" && i+1 < argc) {
            std::string mode_str = argv[++i];
            if (mode_str == "loopback") rec_opt.mode = CaptureMode::LOOPBACK_ONLY;
            else if (mode_str == "microphone") rec_opt.mode = CaptureMode::MICROPHONE_ONLY;
            else if (mode_str == "dual-separate") rec_opt.mode = CaptureMode::DUAL_SEPARATE;
            else if (mode_str == "dual-stereo") rec_opt.mode = CaptureMode::DUAL_STEREO;
            else if (mode_str == "dual-mono") rec_opt.mode = CaptureMode::DUAL_MONO;
            else {
                std::cerr << "Invalid mode: " << mode_str << std::endl;
                return 1;
            }
        }
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --model path        Whisper model file (default: models/ggml-small.en-tdrz.bin)\n";
            std::cout << "  --seconds N         Recording duration in seconds (default: 600)\n";
            std::cout << "  --out wav           Output WAV file (default: meeting.wav)\n";
            std::cout << "  --mode MODE         Capture mode (default: loopback)\n";
            std::cout << "                        loopback      - System audio only\n";
            std::cout << "                        microphone    - Microphone only\n";
            std::cout << "                        dual-separate - Both to separate files\n";
            std::cout << "                        dual-stereo   - Both mixed to stereo (L=mic, R=system)\n";
            std::cout << "                        dual-mono     - Both mixed to mono\n";
            std::cout << "  --mic-device substr Optional microphone device substring filter\n";
            std::cout << "  -h, --help          Show this help message\n";
            std::cout << "\nExamples:\n";
            std::cout << "  " << argv[0] << " --seconds 300 --out call.wav\n";
            std::cout << "  " << argv[0] << " --mode dual-stereo --seconds 120 --out meeting.wav\n";
            std::cout << "  " << argv[0] << " --mode dual-separate --out dual_capture.wav\n";
            std::cout << "  " << argv[0] << " --mode microphone --mic-device \"USB Mic\"\n";
            std::cout << "\nNote: Uses WASAPI for high-quality audio capture on Windows.\n";
            return 0;
        }
    }

    // 1) record using appropriate capture mode
    int rc;
#ifdef _WIN32
    switch (rec_opt.mode) {
        case CaptureMode::LOOPBACK_ONLY:
            rc = record_wasapi_loopback(rec_opt);
            break;
        case CaptureMode::MICROPHONE_ONLY:
            rc = record_microphone_only(rec_opt);
            break;
        case CaptureMode::DUAL_SEPARATE:
        case CaptureMode::DUAL_STEREO:
        case CaptureMode::DUAL_MONO:
            rc = record_dual_audio(rec_opt);
            break;
        default:
            std::cerr << "Unknown capture mode" << std::endl;
            return 1;
    }
#else
    std::cerr << "This application currently only supports Windows with WASAPI.\n";
    return 1;
#endif
    
    if (rc != 0) {
        std::cerr << "record error " << rc << " (check device, permissions, routing)\n";
        return 1;
    }

    // 2) transcribe + diarize
    auto tr = transcribe_whisper_tdrz(model_path, rec_opt.out_path, std::thread::hardware_concurrency());
    if (tr.plain_text.empty()) {
        std::cerr << "transcription failed\n"; return 2;
    }

    // 3) render diarized text
    std::string diarized = render_diarized(tr);

    // 4) summarize with OpenAI
    std::string summary_json = summarize_with_openai(diarized);

    // 5) output
    std::cout << "==== DIARIZED TRANSCRIPT ====\n" << diarized << "\n";
    std::cout << "==== SUMMARY (Responses API JSON) ====\n" << summary_json << "\n";
    return 0;
}
