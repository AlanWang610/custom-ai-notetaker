#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <map>

#include "c-api/c-api.h"

struct SpeakerSegment {
    float start;
    float end;
    int speaker;
    std::string text;
};

class TranscriptionEngine {
private:
    const SherpaOnnxOfflineRecognizer* recognizer;
    const SherpaOnnxVoiceActivityDetector* vad;
    const SherpaOnnxOfflineSpeakerDiarization* diarization;
    std::string modelPath;
    std::string vadModelPath;
    std::string segmentationModelPath;
    std::string embeddingModelPath;
    
public:
    TranscriptionEngine(const std::string& modelDir, const std::string& vadModelFile, 
                       const std::string& segmentationModel, const std::string& embeddingModel) 
        : recognizer(nullptr), vad(nullptr), diarization(nullptr), 
          modelPath(modelDir), vadModelPath(vadModelFile), 
          segmentationModelPath(segmentationModel), embeddingModelPath(embeddingModel) {
        Initialize();
    }
    
    ~TranscriptionEngine() {
        if (recognizer) {
            SherpaOnnxDestroyOfflineRecognizer(recognizer);
        }
        if (vad) {
            SherpaOnnxDestroyVoiceActivityDetector(vad);
        }
        if (diarization) {
            SherpaOnnxDestroyOfflineSpeakerDiarization(diarization);
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
        
        if (!std::filesystem::exists(segmentationModelPath)) {
            std::cerr << "Error: Segmentation model file not found: " << segmentationModelPath << std::endl;
            return false;
        }
        
        if (!std::filesystem::exists(embeddingModelPath)) {
            std::cerr << "Error: Embedding model file not found: " << embeddingModelPath << std::endl;
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
        
        // Configure speaker diarization
        SherpaOnnxOfflineSpeakerDiarizationConfig diarizationConfig;
        memset(&diarizationConfig, 0, sizeof(diarizationConfig));
        diarizationConfig.segmentation.pyannote.model = segmentationModelPath.c_str();
        diarizationConfig.embedding.model = embeddingModelPath.c_str();
        diarizationConfig.clustering.threshold = 0.5f; // Use threshold instead of fixed number of speakers
        
        diarization = SherpaOnnxCreateOfflineSpeakerDiarization(&diarizationConfig);
        
        if (diarization == nullptr) {
            std::cerr << "Error: Failed to create speaker diarization" << std::endl;
            return false;
        }
        
        std::cout << "Transcription engine with VAD and Speaker Diarization initialized successfully" << std::endl;
        std::cout << "ASR Model: " << modelPath << std::endl;
        std::cout << "VAD Model: " << vadModelPath << std::endl;
        std::cout << "Segmentation Model: " << segmentationModelPath << std::endl;
        std::cout << "Embedding Model: " << embeddingModelPath << std::endl;
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
    
    std::vector<SpeakerSegment> TranscribeWithDiarization(const std::string& wavFile) {
        std::vector<SpeakerSegment> result;
        
        if (!recognizer || !vad || !diarization) {
            std::cerr << "Error: Transcription engine not initialized" << std::endl;
            return result;
        }
        
        if (!std::filesystem::exists(wavFile)) {
            std::cerr << "Error: Audio file not found: " << wavFile << std::endl;
            return result;
        }
        
        std::cout << "Transcribing with speaker diarization: " << wavFile << std::endl;
        
        // Read the WAV file
        const SherpaOnnxWave* wave = SherpaOnnxReadWave(wavFile.c_str());
        if (wave == nullptr) {
            std::cerr << "Error: Failed to read WAV file: " << wavFile << std::endl;
            return result;
        }
        
        // Check sample rate
        if (wave->sample_rate != 16000) {
            std::cerr << "Warning: Expected sample rate 16000 Hz, got " << wave->sample_rate << " Hz" << std::endl;
            SherpaOnnxFreeWave(wave);
            return result;
        }
        
        std::cout << "Audio info - Sample rate: " << wave->sample_rate << " Hz, Samples: " << wave->num_samples << std::endl;
        
        // Perform speaker diarization
        auto start_time = std::chrono::high_resolution_clock::now();
        
        const SherpaOnnxOfflineSpeakerDiarizationResult* diarizationResult = 
            SherpaOnnxOfflineSpeakerDiarizationProcess(diarization, wave->samples, wave->num_samples);
        
        if (diarizationResult == nullptr) {
            std::cerr << "Error: Failed to perform speaker diarization" << std::endl;
            SherpaOnnxFreeWave(wave);
            return result;
        }
        
        int32_t num_segments = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(diarizationResult);
        const SherpaOnnxOfflineSpeakerDiarizationSegment* segments = 
            SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(diarizationResult);
        
        std::cout << "Found " << num_segments << " speaker segments" << std::endl;
        
        // Process each speaker segment with transcription
        for (int32_t i = 0; i < num_segments; ++i) {
            float segment_start = segments[i].start;
            float segment_end = segments[i].end;
            int speaker_id = segments[i].speaker;
            
            // Extract audio segment
            int32_t start_sample = static_cast<int32_t>(segment_start * wave->sample_rate);
            int32_t end_sample = static_cast<int32_t>(segment_end * wave->sample_rate);
            int32_t segment_length = end_sample - start_sample;
            
            if (segment_length <= 0) continue;
            
            // Create a copy of the audio segment
            std::vector<float> segment_audio(wave->samples + start_sample, wave->samples + end_sample);
            
            // Transcribe this segment
            const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer);
            SherpaOnnxAcceptWaveformOffline(stream, wave->sample_rate, segment_audio.data(), segment_length);
            SherpaOnnxDecodeOfflineStream(recognizer, stream);
            
            const SherpaOnnxOfflineRecognizerResult* transcriptionResult = SherpaOnnxGetOfflineStreamResult(stream);
            std::string text = transcriptionResult ? transcriptionResult->text : "";
            
            if (!text.empty()) {
                SpeakerSegment segment;
                segment.start = segment_start;
                segment.end = segment_end;
                segment.speaker = speaker_id;
                segment.text = text;
                result.push_back(segment);
                
                std::cout << "Speaker " << speaker_id << " [" << segment_start << "s - " << segment_end << "s]: " << text << std::endl;
            }
            
            // Cleanup
            if (transcriptionResult) {
                SherpaOnnxDestroyOfflineRecognizerResult(transcriptionResult);
            }
            SherpaOnnxDestroyOfflineStream(stream);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::cout << "Speaker diarization and transcription completed in " << duration.count() << " ms" << std::endl;
        
        // Cleanup
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segments);
        SherpaOnnxOfflineSpeakerDiarizationDestroyResult(diarizationResult);
        SherpaOnnxFreeWave(wave);
        
        return result;
    }
    
    bool IsInitialized() const {
        return recognizer != nullptr && vad != nullptr && diarization != nullptr;
    }
};

void PrintUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <wav_file1> [wav_file2] ..." << std::endl;
    std::cout << "Example: " << programName << " recording_20250912_152706_microphone.wav recording_20250912_152706_system.wav" << std::endl;
    std::cout << "The ASR model files should be in: models/sherpa-onnx-moonshine-base-en-int8/" << std::endl;
    std::cout << "The VAD model file should be: models/silero_vad.int8.onnx" << std::endl;
    std::cout << "The segmentation model should be: models/sherpa-onnx-pyannote-segmentation-3-0/model.onnx" << std::endl;
    std::cout << "The embedding model should be: models/nemo_en_titanet_small.onnx" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    // Initialize the transcription engine
    std::string modelDir = "models/sherpa-onnx-moonshine-base-en-int8";
    std::string vadModelFile = "models/silero_vad.int8.onnx";
    std::string segmentationModel = "models/sherpa-onnx-pyannote-segmentation-3-0/model.onnx";
    std::string embeddingModel = "models/nemo_en_titanet_small.onnx";
    TranscriptionEngine engine(modelDir, vadModelFile, segmentationModel, embeddingModel);
    
    if (!engine.IsInitialized()) {
        std::cerr << "Failed to initialize transcription engine" << std::endl;
        return 1;
    }
    
    std::cout << "=== Custom AI Note Taker - Transcription Engine with Speaker Diarization ===" << std::endl;
    std::cout << "Processing " << (argc - 1) << " audio file(s)..." << std::endl;
    std::cout << std::endl;
    
    // Process each audio file
    for (int i = 1; i < argc; i++) {
        std::string wavFile = argv[i];
        std::cout << "[" << i << "/" << (argc - 1) << "] ";
        
        // Try speaker diarization first
        std::vector<SpeakerSegment> segments = engine.TranscribeWithDiarization(wavFile);
        
        if (segments.empty()) {
            std::cout << "No speaker segments found, falling back to VAD-only transcription" << std::endl;
            std::string transcription = engine.TranscribeFile(wavFile);
            
            if (transcription.empty()) {
                std::cout << "Failed to transcribe: " << wavFile << std::endl;
            } else {
                std::cout << "Successfully transcribed: " << wavFile << std::endl;
                std::cout << "Result: \"" << transcription << "\"" << std::endl;
            }
        } else {
            std::cout << "Successfully processed with speaker diarization: " << wavFile << std::endl;
            std::cout << "Found " << segments.size() << " speaker segments" << std::endl;
            
            // Group by speaker
            std::map<int, std::vector<SpeakerSegment>> speakerGroups;
            for (const auto& segment : segments) {
                speakerGroups[segment.speaker].push_back(segment);
            }
            
            std::cout << "Speaker Summary:" << std::endl;
            for (const auto& pair : speakerGroups) {
                int speakerId = pair.first;
                const auto& speakerSegments = pair.second;
                std::cout << "  Speaker " << speakerId << " (" << speakerSegments.size() << " segments):" << std::endl;
                
                for (const auto& segment : speakerSegments) {
                    std::cout << "    [" << segment.start << "s - " << segment.end << "s]: " << segment.text << std::endl;
                }
            }
        }
        
        std::cout << std::endl;
    }
    
    std::cout << "Transcription process completed." << std::endl;
    return 0;
}
