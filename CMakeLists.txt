cmake_minimum_required(VERSION 3.10)
project(CustomAINoteTaker)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executable
add_executable(record src/record.cpp)

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

# Compiler-specific options
if(MSVC)
    target_compile_options(record PRIVATE /W4)
else()
    target_compile_options(record PRIVATE -Wall -Wextra -Wpedantic)
endif()
