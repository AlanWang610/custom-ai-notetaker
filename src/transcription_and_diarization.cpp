#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

#include "c-api/c-api.h"

class TranscriptionEngine {
private:
    const SherpaOnnxOfflineRecognizer* recognizer;
    std::string modelPath;
    
public:
    TranscriptionEngine(const std::string& modelDir) : recognizer(nullptr), modelPath(modelDir) {
        Initialize();
    }
    
    ~TranscriptionEngine() {
        if (recognizer) {
            SherpaOnnxDestroyOfflineRecognizer(recognizer);
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
        
        std::cout << "Transcription engine initialized successfully with model: " << modelPath << std::endl;
        return true;
    }
    
    std::string TranscribeFile(const std::string& wavFile) {
        if (!recognizer) {
            std::cerr << "Error: Recognizer not initialized" << std::endl;
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
        
        std::cout << "Audio info - Sample rate: " << wave->sample_rate << " Hz, Samples: " << wave->num_samples << std::endl;
        
        // Create offline stream
        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer);
        if (stream == nullptr) {
            std::cerr << "Error: Failed to create offline stream" << std::endl;
            SherpaOnnxFreeWave(wave);
            return "";
        }
        
        // Process the audio
        auto start_time = std::chrono::high_resolution_clock::now();
        
        SherpaOnnxAcceptWaveformOffline(stream, wave->sample_rate, wave->samples, wave->num_samples);
        SherpaOnnxDecodeOfflineStream(recognizer, stream);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Get the result
        const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
        
        std::string transcription = result ? result->text : "";
        
        std::cout << "Transcription completed in " << duration.count() << " ms" << std::endl;
        std::cout << "Transcription: " << transcription << std::endl;
        
        // Cleanup
        if (result) {
            SherpaOnnxDestroyOfflineRecognizerResult(result);
        }
        SherpaOnnxDestroyOfflineStream(stream);
        SherpaOnnxFreeWave(wave);
        
        return transcription;
    }
    
    bool IsInitialized() const {
        return recognizer != nullptr;
    }
};

void PrintUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <wav_file1> [wav_file2] ..." << std::endl;
    std::cout << "Example: " << programName << " recording_20250912_152706_microphone.wav recording_20250912_152706_system.wav" << std::endl;
    std::cout << "The model files should be in: models/sherpa-onnx-moonshine-base-en-int8/" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    // Initialize the transcription engine
    std::string modelDir = "models/sherpa-onnx-moonshine-base-en-int8";
    TranscriptionEngine engine(modelDir);
    
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
