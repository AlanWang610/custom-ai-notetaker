#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

#include "c-api/c-api.h"

class TranscriptionEngine {
private:
    const SherpaOnnxOfflineRecognizer* recognizer;
    const SherpaOnnxVoiceActivityDetector* vad;
    std::string modelPath;
    std::string vadModelPath;
    
public:
    TranscriptionEngine(const std::string& modelDir, const std::string& vadModelFile) 
        : recognizer(nullptr), vad(nullptr), modelPath(modelDir), vadModelPath(vadModelFile) {
        Initialize();
    }
    
    ~TranscriptionEngine() {
        if (recognizer) {
            SherpaOnnxDestroyOfflineRecognizer(recognizer);
        }
        if (vad) {
            SherpaOnnxDestroyVoiceActivityDetector(vad);
        }
    }
    
    bool Initialize() {
        // Check if model files exist
        std::string preprocessor = modelPath + "/preprocess.onnx";
        std::string encoder = modelPath + "/encode.int8.onnx";
        std::string uncached_decoder = modelPath + "/uncached_decode.int8.onnx";
        std::string cached_decoder = modelPath + "/cached_decode.int8.onnx";
        std::string tokens = modelPath + "/tokens.txt";
        
        if (!std::filesystem::exists(preprocessor) || 
            !std::filesystem::exists(encoder) || 
            !std::filesystem::exists(uncached_decoder) || 
            !std::filesystem::exists(cached_decoder) || 
            !std::filesystem::exists(tokens)) {
            std::cerr << "Error: Required model files not found in " << modelPath << std::endl;
            return false;
        }
        
        if (!std::filesystem::exists(vadModelPath)) {
            std::cerr << "Error: VAD model file not found: " << vadModelPath << std::endl;
            return false;
        }
        
        // Configure offline model
        SherpaOnnxOfflineModelConfig offline_model_config;
        memset(&offline_model_config, 0, sizeof(offline_model_config));
        offline_model_config.debug = 0;  // Set to 1 for debug output
        offline_model_config.num_threads = 1;
        offline_model_config.provider = "cpu";
        offline_model_config.tokens = tokens.c_str();
        offline_model_config.moonshine.preprocessor = preprocessor.c_str();
        offline_model_config.moonshine.encoder = encoder.c_str();
        offline_model_config.moonshine.uncached_decoder = uncached_decoder.c_str();
        offline_model_config.moonshine.cached_decoder = cached_decoder.c_str();
        
        // Configure recognizer
        SherpaOnnxOfflineRecognizerConfig recognizer_config;
        memset(&recognizer_config, 0, sizeof(recognizer_config));
        recognizer_config.decoding_method = "greedy_search";
        recognizer_config.model_config = offline_model_config;
        
        // Create recognizer
        recognizer = SherpaOnnxCreateOfflineRecognizer(&recognizer_config);
        
        if (recognizer == nullptr) {
            std::cerr << "Error: Failed to create recognizer. Please check your model configuration." << std::endl;
            return false;
        }
        
        // Configure VAD
        SherpaOnnxVadModelConfig vadConfig;
        memset(&vadConfig, 0, sizeof(vadConfig));
        vadConfig.silero_vad.model = vadModelPath.c_str();
        vadConfig.silero_vad.threshold = 0.25f;
        vadConfig.silero_vad.min_silence_duration = 0.5f;
        vadConfig.silero_vad.min_speech_duration = 0.5f;
        vadConfig.silero_vad.max_speech_duration = 10.0f;
        vadConfig.silero_vad.window_size = 512;
        vadConfig.sample_rate = 16000;
        vadConfig.num_threads = 1;
        vadConfig.debug = 0;
        
        vad = SherpaOnnxCreateVoiceActivityDetector(&vadConfig, 30);
        
        if (vad == nullptr) {
            std::cerr << "Error: Failed to create VAD" << std::endl;
            return false;
        }
        
        std::cout << "Transcription engine with VAD initialized successfully" << std::endl;
        std::cout << "ASR Model: " << modelPath << std::endl;
        std::cout << "VAD Model: " << vadModelPath << std::endl;
        return true;
    }
    
    std::string TranscribeFile(const std::string& wavFile) {
        if (!recognizer || !vad) {
            std::cerr << "Error: Transcription engine not initialized" << std::endl;
            return "";
        }
        
        if (!std::filesystem::exists(wavFile)) {
            std::cerr << "Error: Audio file not found: " << wavFile << std::endl;
            return "";
        }
        
        std::cout << "Transcribing: " << wavFile << std::endl;
        
        // Read the WAV file
        const SherpaOnnxWave* wave = SherpaOnnxReadWave(wavFile.c_str());
        if (wave == nullptr) {
            std::cerr << "Error: Failed to read WAV file: " << wavFile << std::endl;
            return "";
        }
        
        // Check sample rate
        if (wave->sample_rate != 16000) {
            std::cerr << "Warning: Expected sample rate 16000 Hz, got " << wave->sample_rate << " Hz" << std::endl;
            SherpaOnnxFreeWave(wave);
            return "Error: Unsupported sample rate";
        }
        
        std::cout << "Audio info - Sample rate: " << wave->sample_rate << " Hz, Samples: " << wave->num_samples << std::endl;
        
        // Process audio with VAD
        std::vector<std::string> transcriptions;
        int32_t window_size = 512; // Silero VAD window size
        int32_t i = 0;
        int is_eof = 0;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        while (!is_eof) {
            if (i + window_size < wave->num_samples) {
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, wave->samples + i, window_size);
            } else {
                SherpaOnnxVoiceActivityDetectorFlush(vad);
                is_eof = 1;
            }
            
            while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
                const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(vad);
                
                // Create stream for this segment
                const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer);
                
                // Accept waveform for this segment
                SherpaOnnxAcceptWaveformOffline(stream, wave->sample_rate, segment->samples, segment->n);
                
                // Decode
                SherpaOnnxDecodeOfflineStream(recognizer, stream);
                
                // Get result
                const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
                
                float start = segment->start / 16000.0f;
                float duration = segment->n / 16000.0f;
                float stop = start + duration;
                
                std::string segmentText = result ? result->text : "";
                if (!segmentText.empty()) {
                    transcriptions.push_back(segmentText);
                    std::cout << "Speech segment [" << start << "s - " << stop << "s]: " << segmentText << std::endl;
                }
                
                // Cleanup
                if (result) {
                    SherpaOnnxDestroyOfflineRecognizerResult(result);
                }
                SherpaOnnxDestroyOfflineStream(stream);
                SherpaOnnxDestroySpeechSegment(segment);
                SherpaOnnxVoiceActivityDetectorPop(vad);
            }
            i += window_size;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Combine all transcriptions
        std::string fullTranscription;
        for (size_t j = 0; j < transcriptions.size(); ++j) {
            if (j > 0) {
                fullTranscription += " ";
            }
            fullTranscription += transcriptions[j];
        }
        
        std::cout << "Transcription completed in " << duration.count() << " ms" << std::endl;
        std::cout << "Transcription: " << (fullTranscription.empty() ? "No speech detected" : fullTranscription) << std::endl;
        
        // Cleanup
        SherpaOnnxFreeWave(wave);
        
        return fullTranscription.empty() ? "No speech detected" : fullTranscription;
    }
    
    bool IsInitialized() const {
        return recognizer != nullptr && vad != nullptr;
    }
};

void PrintUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <wav_file1> [wav_file2] ..." << std::endl;
    std::cout << "Example: " << programName << " recording_20250912_152706_microphone.wav recording_20250912_152706_system.wav" << std::endl;
    std::cout << "The ASR model files should be in: models/sherpa-onnx-moonshine-base-en-int8/" << std::endl;
    std::cout << "The VAD model file should be: models/silero_vad.int8.onnx" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    // Initialize the transcription engine
    std::string modelDir = "models/sherpa-onnx-moonshine-base-en-int8";
    std::string vadModelFile = "models/silero_vad.int8.onnx";
    TranscriptionEngine engine(modelDir, vadModelFile);
    
    if (!engine.IsInitialized()) {
        std::cerr << "Failed to initialize transcription engine" << std::endl;
        return 1;
    }
    
    std::cout << "=== Custom AI Note Taker - Transcription Engine ===" << std::endl;
    std::cout << "Processing " << (argc - 1) << " audio file(s)..." << std::endl;
    std::cout << std::endl;
    
    // Process each audio file
    for (int i = 1; i < argc; i++) {
        std::string wavFile = argv[i];
        std::cout << "[" << i << "/" << (argc - 1) << "] ";
        
        std::string transcription = engine.TranscribeFile(wavFile);
        
        if (transcription.empty()) {
            std::cout << "Failed to transcribe: " << wavFile << std::endl;
        } else {
            std::cout << "Successfully transcribed: " << wavFile << std::endl;
            std::cout << "Result: \"" << transcription << "\"" << std::endl;
        }
        
        std::cout << std::endl;
    }
    
    std::cout << "Transcription process completed." << std::endl;
    return 0;
}
