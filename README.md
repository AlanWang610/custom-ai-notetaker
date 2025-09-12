# Custom AI Note Taker

A C++ application for audio recording and processing.

## Project Structure

```
custom-ai-notetaker/
├── source/
│   └── record_audio.cpp    # Main audio recording application
├── models/                 # AI model files
├── CMakeLists.txt         # Build configuration
└── README.md              # This file
```

## Building the Project

### Prerequisites
- CMake 3.10 or higher
- C++17 compatible compiler (GCC, Clang, or MSVC)

### Build Instructions

1. Create a build directory:
   ```bash
   mkdir build
   cd build
   ```

2. Generate build files:
   ```bash
   cmake ..
   ```

3. Build the project:
   ```bash
   cmake --build .
   ```

4. Run the executable:
   ```bash
   # On Windows
   bin/record_audio.exe
   
   # On Linux/macOS
   ./bin/record_audio
   ```

## Development

The main source file is located in `source/record_audio.cpp`. This file contains the basic structure for the audio recording application and can be extended with additional functionality as needed.
