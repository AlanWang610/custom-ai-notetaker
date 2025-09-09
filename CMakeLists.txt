cmake_minimum_required(VERSION 3.20)
project(meetnotes CXX)

set(CMAKE_CXX_STANDARD 17)
add_subdirectory(third_party/whisper.cpp)  # builds libwhisper

# Manual dependency paths
set(PORTAUDIO_ROOT "${CMAKE_SOURCE_DIR}/deps/portaudio/portaudio")
set(LIBSNDFILE_ROOT "${CMAKE_SOURCE_DIR}/deps/libsndfile/libsndfile-1.2.2-win64")
set(CURL_ROOT "${CMAKE_SOURCE_DIR}/deps/curl/curl-8.15.0_7-win64-mingw")

# PortAudio
set(PORTAUDIO_INCLUDE "${PORTAUDIO_ROOT}/include")
find_library(PORTAUDIO_LIB 
    NAMES portaudio portaudio_static
    PATHS "${PORTAUDIO_ROOT}/lib" "${PORTAUDIO_ROOT}/build/Release"
    NO_DEFAULT_PATH
)

# libsndfile
set(SNDFILE_INCLUDE_DIRS "${LIBSNDFILE_ROOT}/include")
find_library(SNDFILE_LIBRARIES
    NAMES sndfile libsndfile-1
    PATHS "${LIBSNDFILE_ROOT}/lib"
    NO_DEFAULT_PATH
)

# CURL
set(CURL_INCLUDE_DIR "${CURL_ROOT}/include")
find_library(CURL_LIBRARY
    NAMES libcurl curl
    PATHS "${CURL_ROOT}/lib"
    NO_DEFAULT_PATH
)

add_executable(meetnotes src/main.cpp)
target_include_directories(meetnotes PRIVATE
  ${PORTAUDIO_INCLUDE} 
  ${SNDFILE_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIR}
  third_party/whisper.cpp)
target_link_libraries(meetnotes PRIVATE
  whisper 
  ${PORTAUDIO_LIB} 
  ${SNDFILE_LIBRARIES} 
  ${CURL_LIBRARY})

# Windows-specific WASAPI libraries
if(WIN32)
  target_link_libraries(meetnotes PRIVATE
    ole32
    oleaut32
    mmdevapi
    audioclient
    avrt)
endif()
