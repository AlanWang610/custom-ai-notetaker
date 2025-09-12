#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <conio.h>
#include <samplerate.h>

// Windows Audio Session API headers
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "avrt.lib")

struct WAVEFILEHEADER {
    char riffHeader[4];
    uint32_t wavSize;
    char waveHeader[4];
    char fmtHeader[4];
    uint32_t fmtChunkSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char dataHeader[4];
    uint32_t dataBytes;
};

class AudioRecorder {
private:
    IMMDeviceEnumerator* deviceEnumerator;
    IMMDevice* defaultRenderDevice;
    IMMDevice* defaultCaptureDevice;
    IAudioClient* renderClient;
    IAudioClient* captureClient;
    IAudioCaptureClient* captureClientInterface;
    IAudioRenderClient* renderClientInterface;
    IAudioClient* loopbackClient;
    IAudioCaptureClient* loopbackCaptureClient;
    
    std::vector<int16_t> microphoneBuffer;
    std::vector<int16_t> systemBuffer;
    std::vector<int16_t> microphoneBufferNative;
    std::vector<int16_t> systemBufferNative;
    std::atomic<bool> recording;
    std::atomic<bool> shouldStop;
    std::thread recordingThread;
    std::thread keyboardThread;
    
    WAVEFORMATEX* microphoneWaveFormat;
    WAVEFORMATEX* systemWaveFormat;
    WAVEFORMATEX* microphoneWaveFormatNative;
    WAVEFORMATEX* systemWaveFormatNative;
    UINT32 microphoneBufferFrameCount;
    UINT32 systemBufferFrameCount;
    
    // libsamplerate state
    SRC_STATE* microphoneSrcState;
    SRC_STATE* systemSrcState;
    
    std::string outputDirectory;
    std::string baseFilename;
    int recordingDurationSeconds;

public:
    AudioRecorder() : deviceEnumerator(nullptr), defaultRenderDevice(nullptr), 
                     defaultCaptureDevice(nullptr), renderClient(nullptr), 
                     captureClient(nullptr), captureClientInterface(nullptr),
                     renderClientInterface(nullptr), loopbackClient(nullptr),
                     loopbackCaptureClient(nullptr), microphoneWaveFormat(nullptr),
                     systemWaveFormat(nullptr), microphoneWaveFormatNative(nullptr),
                     systemWaveFormatNative(nullptr), microphoneBufferFrameCount(0), 
                     systemBufferFrameCount(0), microphoneSrcState(nullptr),
                     systemSrcState(nullptr), recording(false), shouldStop(false), 
                     recordingDurationSeconds(0) {
    }

    ~AudioRecorder() {
        Cleanup();
    }

    HRESULT Initialize(const std::string& outputDir, int duration) {
        outputDirectory = outputDir;
        recordingDurationSeconds = duration;
        
        // Create device enumerator
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, 
                                    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), 
                                    (void**)&deviceEnumerator);
        if (FAILED(hr)) return hr;

        // Get default render device (for loopback)
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultRenderDevice);
        if (FAILED(hr)) return hr;

        // Get default capture device (microphone)
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultCaptureDevice);
        if (FAILED(hr)) {
            std::cerr << "Failed to get default capture device: " << std::hex << hr << std::endl;
            return hr;
        }

        // Initialize render client for loopback
        hr = defaultRenderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&loopbackClient);
        if (FAILED(hr)) return hr;

        // Initialize capture client for microphone
        hr = defaultCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&captureClient);
        if (FAILED(hr)) return hr;

        // Get native formats for both devices
        hr = captureClient->GetMixFormat(&microphoneWaveFormatNative);
        if (FAILED(hr)) return hr;

        hr = loopbackClient->GetMixFormat(&systemWaveFormatNative);
        if (FAILED(hr)) return hr;

        // Create 16 kHz formats for output
        microphoneWaveFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!microphoneWaveFormat) return E_OUTOFMEMORY;
        
        systemWaveFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!systemWaveFormat) {
            CoTaskMemFree(microphoneWaveFormat);
            return E_OUTOFMEMORY;
        }

        // Configure microphone format for 16 kHz, 16-bit, mono
        microphoneWaveFormat->wFormatTag = WAVE_FORMAT_PCM;
        microphoneWaveFormat->nChannels = 1;  // Mono
        microphoneWaveFormat->nSamplesPerSec = 16000;  // 16 kHz
        microphoneWaveFormat->wBitsPerSample = 16;
        microphoneWaveFormat->nBlockAlign = microphoneWaveFormat->nChannels * microphoneWaveFormat->wBitsPerSample / 8;
        microphoneWaveFormat->nAvgBytesPerSec = microphoneWaveFormat->nSamplesPerSec * microphoneWaveFormat->nBlockAlign;
        microphoneWaveFormat->cbSize = 0;

        // Configure system format for 16 kHz, 16-bit, mono
        systemWaveFormat->wFormatTag = WAVE_FORMAT_PCM;
        systemWaveFormat->nChannels = 1;  // Mono for system audio too
        systemWaveFormat->nSamplesPerSec = 16000;  // 16 kHz
        systemWaveFormat->wBitsPerSample = 16;
        systemWaveFormat->nBlockAlign = systemWaveFormat->nChannels * systemWaveFormat->wBitsPerSample / 8;
        systemWaveFormat->nAvgBytesPerSec = systemWaveFormat->nSamplesPerSec * systemWaveFormat->nBlockAlign;
        systemWaveFormat->cbSize = 0;

        // Initialize loopback client with native format
        hr = loopbackClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 
                                      AUDCLNT_STREAMFLAGS_LOOPBACK, 
                                      0, 0, systemWaveFormatNative, nullptr);
        if (FAILED(hr)) return hr;

        // Initialize capture client with native format
        hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 
                                     10000000, 0, microphoneWaveFormatNative, nullptr);
        if (FAILED(hr)) return hr;

        // Get capture client interface
        hr = captureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClientInterface);
        if (FAILED(hr)) return hr;

        // Get loopback capture client interface
        hr = loopbackClient->GetService(__uuidof(IAudioCaptureClient), (void**)&loopbackCaptureClient);
        if (FAILED(hr)) return hr;

        // Get buffer sizes
        hr = captureClient->GetBufferSize(&microphoneBufferFrameCount);
        if (FAILED(hr)) return hr;

        hr = loopbackClient->GetBufferSize(&systemBufferFrameCount);
        if (FAILED(hr)) return hr;

        // Initialize libsamplerate for resampling (both will output mono)
        int error;
        microphoneSrcState = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
        if (!microphoneSrcState) {
            std::cerr << "Failed to initialize microphone resampler: " << src_strerror(error) << std::endl;
            return E_FAIL;
        }

        systemSrcState = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
        if (!systemSrcState) {
            std::cerr << "Failed to initialize system audio resampler: " << src_strerror(error) << std::endl;
            return E_FAIL;
        }

        // Get device names for debugging
        LPWSTR microphoneDeviceId = nullptr;
        LPWSTR systemDeviceId = nullptr;
        defaultCaptureDevice->GetId(&microphoneDeviceId);
        defaultRenderDevice->GetId(&systemDeviceId);
        
        std::wcout << "Audio initialization successful:" << std::endl;
        std::wcout << "Microphone Device ID: " << microphoneDeviceId << std::endl;
        std::wcout << "System Device ID: " << systemDeviceId << std::endl;
        
        CoTaskMemFree(microphoneDeviceId);
        CoTaskMemFree(systemDeviceId);
        
        std::cout << "Microphone (Native):" << std::endl;
        std::cout << "  Sample rate: " << microphoneWaveFormatNative->nSamplesPerSec << " Hz" << std::endl;
        std::cout << "  Channels: " << microphoneWaveFormatNative->nChannels << std::endl;
        std::cout << "  Bits per sample: " << microphoneWaveFormatNative->wBitsPerSample << std::endl;
        std::cout << "  Buffer size: " << microphoneBufferFrameCount << " frames" << std::endl;
        std::cout << "System Audio (Native):" << std::endl;
        std::cout << "  Sample rate: " << systemWaveFormatNative->nSamplesPerSec << " Hz" << std::endl;
        std::cout << "  Channels: " << systemWaveFormatNative->nChannels << std::endl;
        std::cout << "  Bits per sample: " << systemWaveFormatNative->wBitsPerSample << std::endl;
        std::cout << "  Buffer size: " << systemBufferFrameCount << " frames" << std::endl;
        std::cout << "Output will be resampled to 16 kHz" << std::endl;

        return S_OK;
    }

    void StartRecording() {
        recording = true;
        shouldStop = false;
        
        // Start capture
        captureClient->Start();
        loopbackClient->Start();
        
        // Start recording thread
        recordingThread = std::thread(&AudioRecorder::RecordingLoop, this);
        
        // Start keyboard monitoring thread
        keyboardThread = std::thread(&AudioRecorder::KeyboardLoop, this);
        
        std::cout << "Recording started. Press 'q' to stop early or wait for " 
                  << recordingDurationSeconds << " seconds." << std::endl;
    }

    void StopRecording() {
        shouldStop = true;
        recording = false;
        
        if (recordingThread.joinable()) {
            recordingThread.join();
        }
        
        if (keyboardThread.joinable()) {
            keyboardThread.join();
        }
        
        captureClient->Stop();
        loopbackClient->Stop();
        
        SaveToWAV();
    }

    bool IsRecording() const {
        return recording.load();
    }

private:
    void RecordingLoop() {
        auto startTime = std::chrono::steady_clock::now();
        auto endTime = startTime + std::chrono::seconds(recordingDurationSeconds);
        
        std::cout << "Recording loop started..." << std::endl;
        
        while (recording && !shouldStop) {
            auto currentTime = std::chrono::steady_clock::now();
            if (currentTime >= endTime) {
                std::cout << "Recording time completed." << std::endl;
                shouldStop = true;
                break;
            }
            
            // Capture microphone data to native buffer
            CaptureAudioData(captureClientInterface, microphoneBufferFrameCount, microphoneBufferNative, "Microphone", microphoneWaveFormatNative);
            
            // Capture loopback data to native buffer
            CaptureAudioData(loopbackCaptureClient, systemBufferFrameCount, systemBufferNative, "System", systemWaveFormatNative);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        std::cout << "Recording loop ended. Microphone samples (native): " << microphoneBufferNative.size() 
                  << ", System samples (native): " << systemBufferNative.size() << std::endl;
        
        // Resample the audio data
        ResampleAudio();
        
        recording = false;
    }

    void CaptureAudioData(IAudioCaptureClient* client, UINT32 /*bufferSize*/, std::vector<int16_t>& buffer, const std::string& sourceName, WAVEFORMATEX* waveFormat) {
        if (!client) return;
        
        UINT32 packetLength = 0;
        HRESULT hr = client->GetNextPacketSize(&packetLength);
        
        while (SUCCEEDED(hr) && packetLength > 0) {
            BYTE* data;
            UINT32 numFramesAvailable;
            DWORD flags;
            
            hr = client->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                // Always capture data for debugging (remove silence check temporarily)
                if (numFramesAvailable > 0) {
                    // Convert samples to 16-bit PCM based on the format
                    if (waveFormat->wBitsPerSample == 32 && (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || waveFormat->wFormatTag == 65534)) {
                        // Float samples
                        float* floatData = (float*)data;
                        for (UINT32 i = 0; i < numFramesAvailable * waveFormat->nChannels; i++) {
                            // Clamp the value to prevent overflow
                            float sample = floatData[i];
                            if (sample > 1.0f) sample = 1.0f;
                            if (sample < -1.0f) sample = -1.0f;
                            
                            int16_t pcmSample = (int16_t)(sample * 32767.0f);
                            buffer.push_back(pcmSample);
                        }
                    } else if (waveFormat->wBitsPerSample == 16) {
                        // Already 16-bit PCM
                        int16_t* pcmData = (int16_t*)data;
                        for (UINT32 i = 0; i < numFramesAvailable * waveFormat->nChannels; i++) {
                            buffer.push_back(pcmData[i]);
                        }
                    } else {
                        std::cerr << sourceName << " - Unsupported audio format: " << waveFormat->wBitsPerSample << " bits, format: " << waveFormat->wFormatTag << std::endl;
                    }
                }
                
                client->ReleaseBuffer(numFramesAvailable);
            } else {
                std::cerr << sourceName << " - Failed to get audio buffer: " << std::hex << hr << std::endl;
            }
            
            hr = client->GetNextPacketSize(&packetLength);
        }
    }

    void ResampleAudio() {
        std::cout << "Starting audio resampling..." << std::endl;
        
        // Resample microphone audio
        if (!microphoneBufferNative.empty()) {
            ResampleBuffer(microphoneBufferNative, microphoneBuffer, microphoneSrcState, 
                          microphoneWaveFormatNative, microphoneWaveFormat, "Microphone");
        }
        
        // Resample system audio
        if (!systemBufferNative.empty()) {
            ResampleBuffer(systemBufferNative, systemBuffer, systemSrcState, 
                          systemWaveFormatNative, systemWaveFormat, "System");
        }
        
        std::cout << "Resampling completed. Microphone samples (resampled): " << microphoneBuffer.size() 
                  << ", System samples (resampled): " << systemBuffer.size() << std::endl;
    }

    void ResampleBuffer(const std::vector<int16_t>& inputBuffer, std::vector<int16_t>& outputBuffer, 
                       SRC_STATE* srcState, WAVEFORMATEX* inputFormat, WAVEFORMATEX* outputFormat, 
                       const std::string& sourceName) {
        if (inputBuffer.empty()) return;
        
        // Calculate the ratio for resampling
        double ratio = (double)outputFormat->nSamplesPerSec / (double)inputFormat->nSamplesPerSec;
        
        // First, convert multi-channel to mono by averaging channels
        std::vector<float> monoInput;
        size_t inputFrames = inputBuffer.size() / inputFormat->nChannels;
        monoInput.reserve(inputFrames);
        
        for (size_t frame = 0; frame < inputFrames; frame++) {
            float sum = 0.0f;
            for (int ch = 0; ch < inputFormat->nChannels; ch++) {
                size_t sampleIndex = frame * inputFormat->nChannels + ch;
                sum += (float)inputBuffer[sampleIndex] / 32768.0f;
            }
            monoInput.push_back(sum / inputFormat->nChannels);
        }
        
        // Estimate output size for mono
        size_t estimatedOutputFrames = (size_t)(inputFrames * ratio * 1.1);
        outputBuffer.reserve(estimatedOutputFrames);
        
        // Prepare output buffer
        std::vector<float> outputFloat(estimatedOutputFrames);
        
        SRC_DATA srcData;
        srcData.data_in = monoInput.data();
        srcData.data_out = outputFloat.data();
        srcData.input_frames = (long)inputFrames;
        srcData.output_frames = (long)estimatedOutputFrames;
        srcData.src_ratio = ratio;
        srcData.end_of_input = 1;
        
        // Perform resampling
        int error = src_process(srcState, &srcData);
        if (error) {
            std::cerr << sourceName << " resampling error: " << src_strerror(error) << std::endl;
            return;
        }
        
        // Convert back to 16-bit PCM
        size_t outputSamples = srcData.output_frames;
        outputBuffer.resize(outputSamples);
        for (size_t i = 0; i < outputSamples; i++) {
            // Clamp and convert to 16-bit
            float sample = outputFloat[i];
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            outputBuffer[i] = (int16_t)(sample * 32767.0f);
        }
        
        std::cout << sourceName << " resampled: " << inputBuffer.size() << " samples (" 
                  << inputFormat->nChannels << "ch) -> " << outputBuffer.size() 
                  << " samples (1ch) (ratio: " << ratio << ")" << std::endl;
    }

    void KeyboardLoop() {
        while (recording && !shouldStop) {
            if (_kbhit()) {
                int key = _getch();
                if (key == 'q' || key == 'Q') {
                    std::cout << "\nStopping recording early..." << std::endl;
                    shouldStop = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void SaveToWAV() {
        // Generate base filename with timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &time_t);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
        baseFilename = outputDirectory + "/recording_" + oss.str();
        
        // Save microphone recording
        SaveBufferToWAV(microphoneBuffer, baseFilename + "_microphone.wav", "Microphone", microphoneWaveFormat);
        
        // Save system recording
        SaveBufferToWAV(systemBuffer, baseFilename + "_system.wav", "System", systemWaveFormat);
    }

    void SaveBufferToWAV(const std::vector<int16_t>& buffer, const std::string& filename, const std::string& sourceName, WAVEFORMATEX* waveFormat) {
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to create output file: " << filename << std::endl;
            return;
        }
        
        // WAV header
        WAVEFILEHEADER header = {};
        header.riffHeader[0] = 'R';
        header.riffHeader[1] = 'I';
        header.riffHeader[2] = 'F';
        header.riffHeader[3] = 'F';
        header.waveHeader[0] = 'W';
        header.waveHeader[1] = 'A';
        header.waveHeader[2] = 'V';
        header.waveHeader[3] = 'E';
        header.fmtHeader[0] = 'f';
        header.fmtHeader[1] = 'm';
        header.fmtHeader[2] = 't';
        header.fmtHeader[3] = ' ';
        header.fmtChunkSize = 16;
        header.audioFormat = 1; // PCM
        header.numChannels = waveFormat->nChannels;
        header.sampleRate = waveFormat->nSamplesPerSec;
        header.byteRate = waveFormat->nSamplesPerSec * waveFormat->nChannels * 2; // 16-bit
        header.blockAlign = waveFormat->nChannels * 2;
        header.bitsPerSample = 16;
        header.dataHeader[0] = 'd';
        header.dataHeader[1] = 'a';
        header.dataHeader[2] = 't';
        header.dataHeader[3] = 'a';
        header.dataBytes = static_cast<uint32_t>(buffer.size() * sizeof(int16_t));
        header.wavSize = header.dataBytes + 36;
        
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(buffer.data()), 
                  buffer.size() * sizeof(int16_t));
        
        file.close();
        std::cout << sourceName << " recording saved to: " << filename 
                  << " (" << buffer.size() << " samples)" << std::endl;
    }

    void Cleanup() {
        if (microphoneSrcState) {
            src_delete(microphoneSrcState);
            microphoneSrcState = nullptr;
        }
        
        if (systemSrcState) {
            src_delete(systemSrcState);
            systemSrcState = nullptr;
        }
        
        if (microphoneWaveFormat) {
            CoTaskMemFree(microphoneWaveFormat);
            microphoneWaveFormat = nullptr;
        }
        
        if (systemWaveFormat) {
            CoTaskMemFree(systemWaveFormat);
            systemWaveFormat = nullptr;
        }
        
        if (microphoneWaveFormatNative) {
            CoTaskMemFree(microphoneWaveFormatNative);
            microphoneWaveFormatNative = nullptr;
        }
        
        if (systemWaveFormatNative) {
            CoTaskMemFree(systemWaveFormatNative);
            systemWaveFormatNative = nullptr;
        }
        
        if (captureClientInterface) {
            captureClientInterface->Release();
            captureClientInterface = nullptr;
        }
        
        if (loopbackCaptureClient) {
            loopbackCaptureClient->Release();
            loopbackCaptureClient = nullptr;
        }
        
        if (captureClient) {
            captureClient->Release();
            captureClient = nullptr;
        }
        
        if (loopbackClient) {
            loopbackClient->Release();
            loopbackClient = nullptr;
        }
        
        if (defaultCaptureDevice) {
            defaultCaptureDevice->Release();
            defaultCaptureDevice = nullptr;
        }
        
        if (defaultRenderDevice) {
            defaultRenderDevice->Release();
            defaultRenderDevice = nullptr;
        }
        
        if (deviceEnumerator) {
            deviceEnumerator->Release();
            deviceEnumerator = nullptr;
        }
    }
};

void PrintUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <output_directory> <duration_seconds>" << std::endl;
    std::cout << "Example: " << programName << " C:\\Recordings 30" << std::endl;
    std::cout << "Press 'q' during recording to stop early." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    std::string outputDir = argv[1];
    int duration = std::atoi(argv[2]);
    
    if (duration <= 0) {
        std::cerr << "Duration must be a positive number." << std::endl;
        return 1;
    }
    
    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM: " << hr << std::endl;
        return 1;
    }
    
    AudioRecorder recorder;
    hr = recorder.Initialize(outputDir, duration);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize audio recorder: " << hr << std::endl;
        CoUninitialize();
        return 1;
    }
    
    std::cout << "Audio Recording Application" << std::endl;
    std::cout << "=========================" << std::endl;
    std::cout << "Output directory: " << outputDir << std::endl;
    std::cout << "Duration: " << duration << " seconds" << std::endl;
    
    recorder.StartRecording();
    
    // Wait for recording to complete
    while (recorder.IsRecording()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    recorder.StopRecording();
    
    CoUninitialize();
    return 0;
}
