cmake_minimum_required(VERSION 3.10)
project(CustomAINoteTaker)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add executable
add_executable(record_audio source/record_audio.cpp)

# Link Windows libraries
if(WIN32)
    target_link_libraries(record_audio 
        ole32
        oleaut32
        winmm
        uuid
        wmcodecdspuuid
        avrt
    )
endif()

# Set output directory
set_target_properties(record_audio PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Compiler-specific options
if(MSVC)
    target_compile_options(record_audio PRIVATE /W4)
else()
    target_compile_options(record_audio PRIVATE -Wall -Wextra -Wpedantic)
endif()
