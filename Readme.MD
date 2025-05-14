<p align="center">
  <img src="https://img2.storyblok.com/fit-in/0x300/filters:format(webp)/f/108104/368x415/436d2e239c/sky-logo-white.png" alt="Sky: Children of the Light" width="300"/>
</p>

> **Note:** This repo is by [XeTrinityz](https://github.com/XeTrinityz) created from the [original project](https://github.com/lukas0x1/sml-pc) by [Lukas](https://github.com/lukas0x1) which is no longer maintained. This repo includes significant optimizations and enhancements to the original codebase.

## Overview

ThatSkyModLoader is a powerful mod loader for Sky: Children of the Light on PC. It integrates with the game through a Vulkan layer, allowing for dynamic loading and management of mods without modifying the original game files.

## Features

- **Dynamic Mod Loading**: Load and unload mods at runtime
- **User-Friendly Interface**: ImGui-based interface for managing mods
- **Mod Management**: Enable/disable mods individually
- **Custom Font Support**: Configurable font settings for the mod interface
- **Vulkan Integration**: Seamless integration with the game's rendering pipeline

## Prerequisites

- Windows 10 or later
- Visual Studio 2022 with C++ development tools
- CMake 3.20 or higher
- x64 Native Tools Command Prompt for VS 2022

## Installation

### 1. Clone the Repository

```sh
git clone https://github.com/XeTrinityz/ThatSkyModLoader.git
cd ThatSkyModLoader
```

### 2. Extract Dependencies

Before building, extract the compressed library files:

```sh
# Extract libmem.7z in the lib/release directory
7z x lib/release/libmem.7z -o./lib/release/
```

### 3. Build the Project

Due to linking issues with libmem in Visual Studio, build using the command line:

```cmd
# Open x64 Native Tools Command Prompt for VS 2022
mkdir build
cd build
cmake .. -G "NMake Makefiles" -D CMAKE_BUILD_TYPE="Release"
nmake
```

### 4. Installation

After building, the mod loader will be available as `powrprof.dll` in the `lib/release` directory. To install:

1. Copy `powrprof.dll` to your Sky: Children of the Light installation directory
2. Create a `mods` folder in the same directory if it doesn't exist
3. Place your mod files (`.dll`) in the `mods` folder

## Usage

1. Launch Sky: Children of the Light
2. Use the interface to enable/disable mods
3. Enjoy your modded gameplay experience!

## Creating Mods

Mods for ThatSkyModLoader are DLL files that follow a specific API. A basic mod structure includes:

```cpp
// Example mod structure
extern "C" {
    // Required mod info function
    __declspec(dllexport) ModInfo GetModInfo() {
        ModInfo info;
        info.name = "Example Mod";
        info.version = "1.0.0";
        info.author = "Your Name";
        info.description = "A simple example mod";
        return info;
    }
    
    // Called when the mod is loaded
    __declspec(dllexport) bool OnLoad() {
        // Initialization code
        return true;
    }
    
    // Called when the mod is unloaded
    __declspec(dllexport) void OnUnload() {
        // Cleanup code
    }
}
```

Refer to the documentation for a complete API reference.

## Project Structure

- `src/` - Source code files
  - `layer.cpp` - Vulkan layer implementation
  - `mod_loader.cpp` - Core mod loading functionality
  - `menu.cpp` - ImGui interface implementation
  - `main.cpp` - Entry point
- `include/` - Header files
- `lib/` - External libraries

## Contributing

Contributions are welcome! This fork is actively maintained by [XeTrinityz](https://github.com/XeTrinityz). Please feel free to submit a Pull Request.

1. Fork the project
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the terms of the license included with the repository.

## Contact

For questions or issues specific to this optimized fork, please contact [XeTrinityz](https://github.com/XeTrinityz) or open an issue on GitHub.

---

<p align="center">
  Made with ❤️ for the Sky: Children of the Light community
</p>
