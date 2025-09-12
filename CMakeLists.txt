cmake_minimum_required(VERSION 3.10)
project(CustomAINoteTaker)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executables
add_executable(record src/record.cpp)
add_executable(transcribe src/transcription_and_diarization.cpp)

# Find libsamplerate
find_library(SAMPLERATE_LIB 
    NAMES samplerate
    PATHS ${CMAKE_SOURCE_DIR}/deps/libsamplerate-0.2.2-win64/lib
    NO_DEFAULT_PATH
)

find_path(SAMPLERATE_INCLUDE_DIR
    NAMES samplerate.h
    PATHS ${CMAKE_SOURCE_DIR}/deps/libsamplerate-0.2.2-win64/include
    NO_DEFAULT_PATH
)

if(SAMPLERATE_LIB AND SAMPLERATE_INCLUDE_DIR)
    target_include_directories(record PRIVATE ${SAMPLERATE_INCLUDE_DIR})
    target_link_libraries(record ${SAMPLERATE_LIB})
    message(STATUS "Found libsamplerate: ${SAMPLERATE_LIB}")
else()
    message(FATAL_ERROR "libsamplerate not found")
endif()

# Find sherpa-onnx
set(SHERPA_ONNX_ROOT ${CMAKE_SOURCE_DIR}/deps/sherpa-onnx)
set(SHERPA_ONNX_INCLUDE_DIR ${SHERPA_ONNX_ROOT}/sherpa-onnx)
set(SHERPA_ONNX_LIB_DIR ${SHERPA_ONNX_ROOT}/build/lib/Release)

# Find sherpa-onnx libraries
find_library(SHERPA_ONNX_C_API_LIB
    NAMES sherpa-onnx-c-api
    PATHS ${SHERPA_ONNX_LIB_DIR}
    NO_DEFAULT_PATH
)

find_library(SHERPA_ONNX_CORE_LIB
    NAMES sherpa-onnx-core
    PATHS ${SHERPA_ONNX_LIB_DIR}
    NO_DEFAULT_PATH
)

if(SHERPA_ONNX_C_API_LIB AND SHERPA_ONNX_CORE_LIB)
    target_include_directories(transcribe PRIVATE ${SHERPA_ONNX_INCLUDE_DIR})
    
    # Find all sherpa-onnx related libraries
    find_library(SHERPA_ONNX_CXX_API_LIB
        NAMES sherpa-onnx-cxx-api
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(KALDI_NATIVE_FBANK_LIB
        NAMES kaldi-native-fbank-core
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(KALDI_DECODER_LIB
        NAMES kaldi-decoder-core
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(KALDIFST_LIB
        NAMES sherpa-onnx-kaldifst-core
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(ONNXRUNTIME_LIB
        NAMES onnxruntime
        PATHS ${SHERPA_ONNX_ROOT}/build/_deps/onnxruntime-src/lib
        NO_DEFAULT_PATH
    )
    
    find_library(KISSFFT_LIB
        NAMES kissfft-float
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(SIMPLE_SENTENCEPIECE_LIB
        NAMES ssentencepiece_core
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(PIPER_PHONEMIZE_LIB
        NAMES piper_phonemize
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(CPPINYIN_LIB
        NAMES cppinyin_core
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(ESPEAK_NG_LIB
        NAMES espeak-ng
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(CPPJIEBA_LIB
        NAMES cppjieba
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(HCLUST_CPP_LIB
        NAMES hclust_cpp
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(OPENFST_LIB
        NAMES sherpa-onnx-fst
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(FSTFAR_LIB
        NAMES sherpa-onnx-fstfar
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    find_library(UCD_LIB
        NAMES ucd
        PATHS ${SHERPA_ONNX_LIB_DIR}
        NO_DEFAULT_PATH
    )
    
    # Link essential libraries only
    target_link_libraries(transcribe 
        ${SHERPA_ONNX_C_API_LIB} 
        ${SHERPA_ONNX_CORE_LIB}
        ${SHERPA_ONNX_CXX_API_LIB}
        ${KALDI_NATIVE_FBANK_LIB}
        ${KALDI_DECODER_LIB}
        ${KALDIFST_LIB}
        ${ONNXRUNTIME_LIB}
        ${KISSFFT_LIB}
        ${SIMPLE_SENTENCEPIECE_LIB}
        ${PIPER_PHONEMIZE_LIB}
        ${CPPINYIN_LIB}
        ${ESPEAK_NG_LIB}
        ${UCD_LIB}
        ${OPENFST_LIB}
        ${FSTFAR_LIB}
    )
    
    # Set runtime library to match sherpa-onnx (MT)
    set_target_properties(transcribe PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    
    # Force static runtime for all configurations
    target_compile_options(transcribe PRIVATE /MT$<$<CONFIG:Debug>:d>)
    
    message(STATUS "Found sherpa-onnx: ${SHERPA_ONNX_C_API_LIB}")
else()
    message(FATAL_ERROR "sherpa-onnx libraries not found")
endif()

# Link Windows libraries
if(WIN32)
    target_link_libraries(record 
        ole32
        oleaut32
        winmm
        uuid
        wmcodecdspuuid
        avrt
    )
endif()

# Set output directory
set_target_properties(record PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

set_target_properties(transcribe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Compiler-specific options
if(MSVC)
    target_compile_options(record PRIVATE /W4)
    target_compile_options(transcribe PRIVATE /W4)
else()
    target_compile_options(record PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(transcribe PRIVATE -Wall -Wextra -Wpedantic)
endif()
