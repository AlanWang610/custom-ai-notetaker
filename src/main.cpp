// main.cpp
// Build deps: whisper.cpp (as submodule, provides whisper.h + libwhisper),
// PortAudio, libsndfile, libcurl.
// Example build (Linux/macOS):
//   c++ -std=c++17 main.cpp -Ithird_party/whisper.cpp -Lbuild/whisper -lwhisper \
//       -lsndfile -lportaudio -lcurl -o main
// Run:
//   OPENAI_API_KEY=sk-... ./main --model models/ggml-small.en-tdrz.bin --seconds 600 --out meeting.wav
// Notes:
//   • Use a *-tdrz* model for local diarization (TinyDiarize). 16-kHz mono PCM WAV input.
//   • Select a capture device with --device "substring" (default: system default input).

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

#include <curl/curl.h>
#include <portaudio.h>
#include <sndfile.h>

#include "whisper.h"

// Windows WASAPI includes
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiosessiontypes.h>
#include <avrt.h>
#include <comdef.h>
#include <comip.h>
#include <functiondiscoverykeys_devpkey.h>
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

static size_t curl_sink(char* ptr, size_t sz, size_t nm, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, sz*nm);
    return sz*nm;
}

// -------------------- audio capture (PortAudio + libsndfile) --------------------
struct CaptureOpts {
    int sample_rate = 16000;
    int channels = 1;                  // mono for Whisper
    int seconds = 600;                 // default 10 min
    std::string out_path = "meeting.wav";
    std::string device_substr;         // optional substring match
};

// Options for WASAPI recording
struct RecordOptions {
    int seconds = 600;
    std::string out_path = "meeting.wav";
};

static int find_input_device(const std::string& substr) {
    int def = Pa_GetDefaultInputDevice();
    if (substr.empty()) return def;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di || di->maxInputChannels <= 0) continue;
        std::string name = di->name ? di->name : "";
        if (name.find(substr) != std::string::npos) return i;
    }
    return def;
}

static int record_to_wav(const CaptureOpts& opt) {
    PaError err = Pa_Initialize();
    if (err != paNoError) return 10;

    PaStream* stream = nullptr;
    PaStreamParameters in;
    std::memset(&in, 0, sizeof(in));
    in.device = find_input_device(opt.device_substr);
    if (in.device == paNoDevice) { Pa_Terminate(); return 11; }
    in.channelCount = opt.channels;
    in.sampleFormat = paInt16;
    in.suggestedLatency = Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;

    err = Pa_OpenStream(&stream, &in, nullptr, opt.sample_rate,
                        512, paClipOff, nullptr, nullptr);
    if (err != paNoError) { Pa_Terminate(); return 12; }
    err = Pa_StartStream(stream);
    if (err != paNoError) { Pa_CloseStream(stream); Pa_Terminate(); return 13; }

    SF_INFO info{};
    info.samplerate = opt.sample_rate;
    info.channels   = opt.channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* sf = sf_open(opt.out_path.c_str(), SFM_WRITE, &info);
    if (!sf) { Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate(); return 14; }

    std::vector<int16_t> buf(512 * opt.channels);
    int total_frames = opt.seconds * opt.sample_rate;
    while (total_frames > 0) {
        err = Pa_ReadStream(stream, buf.data(), 512);
        if (err != paNoError) break;
        sf_write_short(sf, buf.data(), (sf_count_t)buf.size());
        total_frames -= 512;
    }

    sf_close(sf);
    Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate();
    return 0;
}

#ifdef _WIN32
// -------------------- WASAPI Loopback Audio Recorder --------------------
class WASAPILoopbackRecorder {
private:
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    
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
        // Initialize in loopback mode
        return audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            10000000,  // 1 second buffer
            0,
            waveFormat,
            nullptr
        );
    }
    
    HRESULT GetCaptureClient() {
        return audioClient->GetService(__uuidof(IAudioCaptureClient),
                                     (void**)&captureClient);
    }
    
    void RecordingThreadFunc() {
        HRESULT hr;
        HANDLE audioTask = AvSetMmThreadCharacteristics(L"Audio", nullptr);
        
        hr = audioClient->Start();
        if (FAILED(hr)) {
            std::cerr << "Failed to start audio client: " << std::hex << hr << std::endl;
            return;
        }
        
        while (!stopFlag.load()) {
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
            
            while (packetLength != 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                
                if (framesAvailable > 0) {
                    // Convert to float if needed and store
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
                
                hr = captureClient->ReleaseBuffer(framesAvailable);
                if (FAILED(hr)) break;
                
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }
            
            Sleep(1); // Small sleep to prevent busy waiting
        }
        
        audioClient->Stop();
        if (audioTask) AvRevertMmThreadCharacteristics(audioTask);
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
#endif // _WIN32

// -------------------- WASAPI Loopback Recording Function --------------------
#ifdef _WIN32
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
    SF_INFO info{};
    info.samplerate = static_cast<int>(sampleRate);
    info.channels   = static_cast<int>(channels);
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    
    SNDFILE* sf = sf_open(opt.out_path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        std::cerr << "Failed to open output file: " << opt.out_path << std::endl;
        return 2;
    }
    
    std::cout << "Starting WASAPI loopback recording for " << opt.seconds << " seconds..." << std::endl;
    std::cout << "Recording system audio output to: " << opt.out_path << std::endl;
    std::cout << "Make sure some audio is playing for best results!" << std::endl;
    
    if (!recorder.StartRecording()) {
        std::cerr << "Failed to start recording" << std::endl;
        sf_close(sf);
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
                sample = std::max(-1.0f, std::min(1.0f, sample));
                int16Data.push_back(static_cast<int16_t>(sample * 32767.0f));
            }
            
            // Write to file
            sf_write_short(sf, int16Data.data(), static_cast<sf_count_t>(int16Data.size()));
            
            // Clear the buffer to avoid writing duplicate data
            recorder.ClearAudioData();
        }
    }
    
    recorder.StopRecording();
    sf_close(sf);
    
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
    SF_INFO si{}; SNDFILE* sf = sf_open(wav_path.c_str(), SFM_READ, &si);
    if (!sf) return out;
    std::vector<short> pcm16((size_t)si.frames * si.channels);
    sf_read_short(sf, pcm16.data(), (sf_count_t)pcm16.size());
    sf_close(sf);

    // to mono float32
    std::vector<float> mono; mono.reserve(si.frames);
    if (si.channels == 1) {
        mono.resize(si.frames);
        for (sf_count_t i = 0; i < si.frames; ++i) mono[i] = pcm16[(size_t)i] / 32768.0f;
    } else {
        mono.resize(si.frames);
        for (sf_count_t i = 0; i < si.frames; ++i) {
            int idx = (int)i * si.channels;
            int sum = 0; for (int c = 0; c < si.channels; ++c) sum += pcm16[idx+c];
            mono[i] = (sum / (float)si.channels) / 32768.0f;
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

// -------------------- OpenAI summary (Responses API via libcurl) --------------------
static std::string summarize_with_openai(const std::string& transcript) {
    std::string key = get_openai_api_key();
    if (key.empty()) return "{\"error\":\"OPENAI_API_KEY not set\"}";
    std::string payload;
    // Structured output prompt. Keep small; model does the rest.
    std::string sys = "Summarize the meeting into JSON: {decisions[], action_items[{owner,task,due?}], risks[], questions[]}. Keep it concise.";
    payload.reserve(transcript.size() + 512);
    payload  = "{"
               "\"model\":\"gpt-4o-mini\","
               "\"input\":["
                 "{\"role\":\"system\",\"content\":\"" + json_escape(sys) + "\"},"
                 "{\"role\":\"user\",\"content\":\"" + json_escape(transcript) + "\"}"
               "]"
               "}";

    CURL* curl = curl_easy_init();
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    std::string auth = std::string("Authorization: Bearer ") + key;
    hdrs = curl_slist_append(hdrs, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/responses");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        std::ostringstream e; e << "{\"error\":\"curl:" << rc << "\"}";
        return e.str();
    }
    return resp; // caller can parse "output_text" or JSON as needed
}

// -------------------- main --------------------
int main(int argc, char** argv) {
    // defaults
    std::string model_path = "models/ggml-small.en-tdrz.bin";
    CaptureOpts cap;
    bool use_wasapi_loopback = false;
    
    // args
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--model" && i+1 < argc) model_path = argv[++i];
        else if (a == "--seconds" && i+1 < argc) cap.seconds = std::atoi(argv[++i]);
        else if (a == "--out" && i+1 < argc) cap.out_path = argv[++i];
        else if (a == "--device" && i+1 < argc) cap.device_substr = argv[++i];
        else if (a == "--loopback") use_wasapi_loopback = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --model path      Whisper model file (default: models/ggml-small.en-tdrz.bin)\n";
            std::cout << "  --seconds N       Recording duration in seconds (default: 600)\n";
            std::cout << "  --out wav         Output WAV file (default: meeting.wav)\n";
            std::cout << "  --device substr   Audio device substring (PortAudio mode only)\n";
            std::cout << "  --loopback        Use WASAPI loopback to capture system audio (Windows only)\n";
            std::cout << "  -h, --help        Show this help message\n";
            std::cout << "\nExamples:\n";
            std::cout << "  " << argv[0] << " --loopback --seconds 300 --out call.wav\n";
            std::cout << "  " << argv[0] << " --model models/ggml-base.en.bin --loopback\n";
            return 0;
        }
    }

    // 1) record
    int rc;
#ifdef _WIN32
    if (use_wasapi_loopback) {
        // Convert CaptureOpts to RecordOptions for WASAPI
        RecordOptions rec_opt;
        rec_opt.seconds = cap.seconds;
        rec_opt.out_path = cap.out_path;
        // WASAPI will determine sample rate and channels automatically
        
        rc = record_wasapi_loopback(rec_opt);
    } else {
        rc = record_to_wav(cap);
    }
#else
    if (use_wasapi_loopback) {
        std::cerr << "WASAPI loopback is only available on Windows. Using PortAudio instead.\n";
    }
    rc = record_to_wav(cap);
#endif
    
    if (rc != 0) {
        std::cerr << "record error " << rc << " (check device, permissions, routing)\n";
        return 1;
    }

    // 2) transcribe + diarize
    auto tr = transcribe_whisper_tdrz(model_path, cap.out_path, std::thread::hardware_concurrency());
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
