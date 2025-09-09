cmake_minimum_required(VERSION 3.20)
project(meetnotes CXX)

set(CMAKE_CXX_STANDARD 17)
add_subdirectory(third_party/whisper.cpp)  # builds libwhisper

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
find_path(PORTAUDIO_INCLUDE portaudio.h)
find_library(PORTAUDIO_LIB portaudio)
find_package(CURL REQUIRED)

add_executable(meetnotes src/main.cpp)
target_include_directories(meetnotes PRIVATE
  ${PORTAUDIO_INCLUDE} ${SNDFILE_INCLUDE_DIRS}
  third_party/whisper.cpp)
target_link_libraries(meetnotes PRIVATE
  whisper ${PORTAUDIO_LIB} ${SNDFILE_LIBRARIES} CURL::libcurl)
