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
#include <iostream>
#include <sstream>
#include <algorithm>

#include <curl/curl.h>
#include <portaudio.h>
#include <sndfile.h>

#include "whisper.h"

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
    int seconds = 300;                 // default 5 min
    std::string out_path = "meeting.wav";
    std::string device_substr;         // optional substring match
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

// -------------------- OpenAI summary (Responses API via libcurl) --------------------
static std::string summarize_with_openai(const std::string& transcript) {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) return "{\"error\":\"OPENAI_API_KEY not set\"}";
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
    // args
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--model" && i+1 < argc) model_path = argv[++i];
        else if (a == "--seconds" && i+1 < argc) cap.seconds = std::atoi(argv[++i]);
        else if (a == "--out" && i+1 < argc) cap.out_path = argv[++i];
        else if (a == "--device" && i+1 < argc) cap.device_substr = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [--model path] [--seconds N] [--out wav] [--device substring]\n";
            return 0;
        }
    }

    // 1) record
    int rc = record_to_wav(cap);
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
