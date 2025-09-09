# TinyDiarize Speaker Diarization Guide

## 🎙️ **What is Speaker Diarization?**

Speaker diarization is the process of partitioning audio streams into homogeneous segments according to speaker identity. In simpler terms, it answers the question **"who spoke when?"** in audio recordings.

Our implementation uses **TinyDiarize**, a specialized Whisper model fine-tuned specifically for speaker diarization tasks.

## 🚀 **TinyDiarize Integration**

### **Model Information**
- **Source**: [akashmjn/tinydiarize-whisper.cpp](https://huggingface.co/akashmjn/tinydiarize-whisper.cpp)
- **Model**: `ggml-small.en-tdrz.bin` (465MB)
- **License**: MIT
- **Language**: English
- **Special Feature**: Outputs `[SPEAKER TURN]` markers in transcription

### **How TinyDiarize Works**
1. **Audio Processing**: Uses Whisper's speech recognition capabilities
2. **Speaker Detection**: Identifies speaker changes in the audio stream
3. **Marker Insertion**: Inserts `[SPEAKER TURN]` tokens at speaker boundaries
4. **Text Segmentation**: Our code splits on these markers to create speaker segments

## 🔧 **Configuration**

The application is now configured to use TinyDiarize by default:

```cpp
// Whisper parameters for TinyDiarize
wparams.tdrz_enable = true;               // Enable TinyDiarize speaker-turn detection
std::string model_path = "models/ggml-small.en-tdrz.bin";  // Default model
```

## 📋 **Usage Examples**

### **Basic Diarization Recording**
```powershell
# Record meeting with WASAPI loopback and diarization
.\meetnotes.exe --loopback --seconds 600

# Record with microphone input and diarization
.\meetnotes.exe --seconds 300 --out interview.wav
```

### **Custom Model Usage**
```powershell
# Explicitly specify the TinyDiarize model
.\meetnotes.exe --model models/ggml-small.en-tdrz.bin --loopback --seconds 600
```

## 🎯 **Output Format**

### **Diarized Transcript Example**
```
Speaker 1: Welcome everyone to today's meeting. Let's start by reviewing the quarterly results.

Speaker 2: Thank you for having me. I'd like to present our sales figures first.

Speaker 1: That sounds great. Please go ahead.

Speaker 3: Before we begin, can we address the budget concerns from last week?

Speaker 2: Absolutely, that's actually part of my presentation.
```

### **Raw Whisper Output**
The model produces text with embedded markers:
```
Welcome everyone to today's meeting. Let's start by reviewing the quarterly results. [SPEAKER TURN] Thank you for having me. I'd like to present our sales figures first. [SPEAKER TURN] That sounds great. Please go ahead. [SPEAKER TURN] Before we begin, can we address the budget concerns from last week? [SPEAKER TURN] Absolutely, that's actually part of my presentation.
```

### **Processing Logic**
Our `render_diarized()` function:
1. Concatenates all Whisper segments
2. Splits text on `[SPEAKER TURN]` markers
3. Assigns sequential speaker numbers
4. Formats as "Speaker N: [text]"

## 🎤 **Best Practices for Diarization**

### **Audio Quality Requirements**
- **Clear speech**: Multiple speakers should be clearly audible
- **Minimal overlap**: Avoid simultaneous speaking when possible
- **Good signal-to-noise ratio**: Reduce background noise
- **Stable volume**: Consistent audio levels across speakers

### **Recording Tips**
- **Use WASAPI loopback** for online meetings (perfect quality)
- **16kHz mono** is optimal for Whisper processing
- **Longer segments** (>30 seconds) improve diarization accuracy
- **Natural pauses** between speakers help with detection

### **Meeting Types That Work Well**
- ✅ **Structured meetings** with clear turn-taking
- ✅ **Interviews** with distinct questioner/respondent roles  
- ✅ **Panel discussions** with moderated speaking
- ✅ **Phone calls** with clear speaker separation

### **Challenging Scenarios**
- ❌ **Rapid back-and-forth** conversations
- ❌ **Overlapping speech** (multiple people talking simultaneously)
- ❌ **Very similar voices** (hard to distinguish)
- ❌ **Background chatter** or noise

## 🧠 **Technical Details**

### **TinyDiarize Model Specifications**
- **Base Model**: OpenAI Whisper `small.en`
- **Fine-tuning**: Specialized for speaker turn detection
- **Output Format**: Standard transcription with `[SPEAKER TURN]` markers
- **Memory Usage**: ~1.0 GB during processing
- **Processing Speed**: Similar to standard Whisper small model

### **Speaker Turn Detection**
The model has been trained to identify:
- **Natural speaker boundaries** (pauses, intonation changes)
- **Acoustic speaker differences** (voice characteristics)
- **Linguistic patterns** (speaking style changes)

### **Accuracy Expectations**
- **Good**: 2-4 speakers with clear speech
- **Excellent**: Structured conversations with natural pauses
- **Challenging**: More than 4 speakers or overlapping speech

## 🔍 **Troubleshooting**

### **No Speaker Separation**
- **Issue**: All text assigned to one speaker
- **Solutions**:
  - Ensure using `ggml-small.en-tdrz.bin` model
  - Verify `tdrz_enable = true` in configuration
  - Check audio quality (clear speaker differences)

### **Too Many Speaker Changes**
- **Issue**: Every sentence becomes a new speaker
- **Solutions**:
  - Improve audio quality (reduce noise)
  - Use longer recording segments
  - Check for audio artifacts or compression issues

### **Missing Speakers**
- **Issue**: Multiple speakers merged into one
- **Solutions**:
  - Ensure speakers have distinct voices
  - Check audio levels (quiet speakers may be missed)
  - Verify natural pauses between speakers

## 📊 **Performance Comparison**

| Model | Size | Diarization | Accuracy | Speed | Memory |
|-------|------|-------------|----------|-------|---------|
| `ggml-base.en` | 142MB | ❌ No | High | Fast | ~0.5GB |
| `ggml-small.en-tdrz` | 465MB | ✅ Yes | High+ | Medium | ~1.0GB |
| `ggml-medium.en` | 1.5GB | ❌ No | Higher | Slow | ~2.0GB |

## 🎉 **Benefits for Meeting Notes**

1. **Clear Attribution**: Know exactly who said what
2. **Better Summaries**: AI can analyze per-speaker contributions
3. **Meeting Insights**: Track speaking time, participation patterns
4. **Action Items**: Assign tasks to specific speakers
5. **Professional Output**: Clean, organized transcripts

## 🔄 **Integration with OpenAI**

The diarized transcript gets sent to OpenAI's API for summarization, which can now:
- **Identify key contributors** in meetings
- **Summarize by speaker** role or contribution
- **Track decisions** and who made them
- **Generate speaker-specific** action items

Your AI notetaker now provides professional-grade speaker diarization using the specialized TinyDiarize model! 🎉

---

**Model Credit**: TinyDiarize model by [akashmjn](https://huggingface.co/akashmjn/tinydiarize-whisper.cpp) - A fine-tuned Whisper model specifically designed for speaker diarization tasks.
