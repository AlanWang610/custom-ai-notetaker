cmake_minimum_required(VERSION 3.20)
project(meetnotes CXX)

set(CMAKE_CXX_STANDARD 17)
add_subdirectory(third_party/whisper.cpp)  # builds libwhisper

add_executable(meetnotes src/main.cpp)
target_include_directories(meetnotes PRIVATE
  third_party/whisper.cpp)
target_link_libraries(meetnotes PRIVATE
  whisper)

# Windows-specific WASAPI libraries (now required, not optional)
if(WIN32)
  target_link_libraries(meetnotes PRIVATE
    ole32
    oleaut32
    mmdevapi
    avrt
    wininet)  # For HTTP requests (replacing CURL)
else()
  message(FATAL_ERROR "This application currently only supports Windows with WASAPI audio capture")
endif()
