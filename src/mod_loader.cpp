#include <cstddef>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <system_error>

#include "include/mod_loader.h"

// Static member initialization
std::vector<ModItem> ModLoader::mods;

std::string ModLoader::GetModsDirectory() {
    try {
        const std::string modsFolder = "mods";
        char buffer[MAX_PATH];
        
        if (GetModuleFileNameA(NULL, buffer, MAX_PATH) == 0) {
            throw std::system_error(GetLastError(), std::system_category(), "Failed to get module filename");
        }
        
        std::string::size_type pos = std::string(buffer).find_last_of("\\/");
        if (pos == std::string::npos) {
            throw std::runtime_error("Failed to find directory separator in path");
        }
        
        return std::string(buffer).substr(0, pos) + "\\" + modsFolder;
    } catch (const std::exception& e) {
        std::cout << "Error getting mods directory: " << e.what() << std::endl;
        return "mods"; // Fallback to relative path
    }
}

bool ModLoader::EnsureModsDirectoryExists(const std::string& directory) {
    try {
        DWORD attributes = GetFileAttributesA(directory.c_str());
        
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            std::cout << "Creating mods directory: " << directory << std::endl;
            if (!CreateDirectoryA(directory.c_str(), NULL)) {
                throw std::system_error(GetLastError(), std::system_category(), "Failed to create mods directory");
            }
            return true;
        }
        
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            throw std::runtime_error("Mods path exists but is not a directory");
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cout << "Error ensuring mods directory exists: " << e.what() << std::endl;
        return false;
    }
}

bool ModLoader::LoadModFromFile(const std::string& filePath) {
    try {
        std::cout << "Loading mod: " << filePath << std::endl;
        
        HMODULE hModule = LoadLibraryA(filePath.c_str());
        if (!hModule) {
            throw std::system_error(GetLastError(), std::system_category(), "Failed to load DLL");
        }
        
        mods.emplace_back(hModule);
        ModItem& item = mods.back();
        
        // Load function pointers
        item.start = reinterpret_cast<StartFn>(GetProcAddress(hModule, "Start"));
        item.onDisable = reinterpret_cast<OnDisableFn>(GetProcAddress(hModule, "onDisable"));
        item.onEnable = reinterpret_cast<OnEnableFn>(GetProcAddress(hModule, "onEnable"));
        item.getInfo = reinterpret_cast<GetModInfoFn>(GetProcAddress(hModule, "GetModInfo"));
        item.render = reinterpret_cast<RenderFn>(GetProcAddress(hModule, "Render"));
        
        // Initialize mod info and start if available
        if (item.getInfo) {
            item.getInfo(item.info);
            std::cout << "Loaded mod: " << item.info.name << " v" << item.info.version << " by " << item.info.author << std::endl;
        } else {
            std::cout << "Warning: Mod does not provide GetModInfo function" << std::endl;
        }
        
        if (item.start) {
            item.start();
            std::cout << "Started mod: " << (item.info.name.empty() ? filePath : item.info.name) << std::endl;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cout << "Error loading mod " << filePath << ": " << e.what() << std::endl;
        return false;
    }
}

void ModLoader::LoadMods() {
    try {
        std::cout << "Starting mod loading process" << std::endl;
        
        // Get and ensure mods directory exists
        std::string modsDirectory = GetModsDirectory();
        if (!EnsureModsDirectoryExists(modsDirectory)) {
            std::cout << "Failed to ensure mods directory exists, mod loading aborted" << std::endl;
            return;
        }
        
        // Clear existing mods if reloading
        if (!mods.empty()) {
            std::cout << "Unloading existing mods before loading new ones" << std::endl;
            UnloadAllMods();
        }
        
        // Load all DLL files from the mods directory
        size_t loadedCount = 0;
        size_t failedCount = 0;
        
        for (const auto& entry : std::filesystem::directory_iterator(modsDirectory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dll") {
                std::string filePath = entry.path().string();
                if (LoadModFromFile(filePath)) {
                    loadedCount++;
                } else {
                    failedCount++;
                }
            }
        }
        
        std::cout << "Mod loading complete. Loaded: " << loadedCount << ", Failed: " << failedCount << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Critical error during mod loading: " << e.what() << std::endl;
    }
}

size_t ModLoader::GetModCount() {
    return mods.size();
}

const ModInfo& ModLoader::GetModInfo(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        static ModInfo emptyInfo;
        std::cout << "Error: Attempted to access mod info with invalid index: " << index << std::endl;
        return emptyInfo;
    }
    return mods[index].info;
}

void ModLoader::Render(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        std::cout << "Error: Attempted to render mod with invalid index: " << index << std::endl;
        return;
    }
    
    try {
        if (mods[index].enabled && mods[index].render) {
            mods[index].render();
        }
    } catch (const std::exception& e) {
        std::cout << "Error rendering mod " << mods[index].info.name << ": " << e.what() << std::endl;
    }
}

void ModLoader::EnableMod(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        std::cout << "Error: Attempted to enable mod with invalid index: " << index << std::endl;
        return;
    }
    
    try {
        // Always call onEnable when requested, regardless of current state
        if (mods[index].onEnable) {
            mods[index].onEnable();
            mods[index].enabled = true;
        } else {
            std::cout << "Mod does not have an onEnable function: " << mods[index].info.name << std::endl;
            // Still mark as enabled even if there's no onEnable function
            mods[index].enabled = true;
        }
    } catch (const std::exception& e) {
        std::cout << "Error enabling mod " << mods[index].info.name << ": " << e.what() << std::endl;
    }
}

void ModLoader::DisableMod(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        std::cout << "Error: Attempted to disable mod with invalid index: " << index << std::endl;
        return;
    }
    
    try {
        // Always call onDisable when requested, regardless of current state
        if (mods[index].onDisable) {
            mods[index].onDisable();
            mods[index].enabled = false;
        } else {
            std::cout << "Mod does not have an onDisable function: " << mods[index].info.name << std::endl;
            // Still mark as disabled even if there's no onDisable function
            mods[index].enabled = false;
        }
    } catch (const std::exception& e) {
        std::cout << "Error disabling mod " << mods[index].info.name << ": " << e.what() << std::endl;
    }
}

bool& ModLoader::GetModEnabled(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        static bool dummyEnabled = false;
        std::cout << "Error: Attempted to access mod enabled status with invalid index: " << index << std::endl;
        return dummyEnabled;
    }
    return mods[index].enabled;
}

std::string_view ModLoader::GetModName(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        static std::string emptyName = "<invalid mod>";
        std::cout << "Error: Attempted to access mod name with invalid index: " << index << std::endl;
        return emptyName;
    }
    return mods[index].info.name;
}

void ModLoader::RenderAll() {
    try {
        for (size_t i = 0; i < mods.size(); i++) {
            Render(static_cast<int>(i));
        }
    } catch (const std::exception& e) {
        std::cout << "Error in RenderAll: " << e.what() << std::endl;
    }
}

std::string ModLoader::toString(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        return "Error: Invalid mod index";
    }
    
    try {
        std::stringstream ss;
        ss << "Information" << "\n";
        ss << "Name: " << mods[index].info.name << "\n";
        ss << "Version: " << mods[index].info.version << "\n";
        ss << "Author: " << mods[index].info.author << "\n";
        ss << "Details: " << mods[index].info.description << "\n";
        return ss.str();
    } catch (const std::exception& e) {
        std::cout << "Error generating string representation for mod " << index << ": " << e.what() << std::endl;
        return "Error generating mod information";
    }
}

void ModLoader::UnloadAllMods() {
    try {
        std::cout << "Unloading all mods" << std::endl;
        
        for (size_t i = 0; i < mods.size(); i++) {
            try {
                // Disable the mod first if it's enabled
                if (mods[i].enabled && mods[i].onDisable) {
                    mods[i].onDisable();
                }
                
                // Free the library
                if (mods[i].hModule) {
                    FreeLibrary(mods[i].hModule);
                    std::cout << "Unloaded mod: " << mods[i].info.name << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "Error unloading mod " << mods[i].info.name << ": " << e.what() << std::endl;
            }
        }
        
        mods.clear();
        std::cout << "All mods unloaded" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Critical error during mod unloading: " << e.what() << std::endl;
    }
}

bool ModLoader::ReloadMod(int index) {
    if (index < 0 || index >= static_cast<int>(mods.size())) {
        std::cout << "Error: Attempted to reload mod with invalid index: " << index << std::endl;
        return false;
    }
    
    try {
        // Store the file path
        char filePath[MAX_PATH];
        if (GetModuleFileNameA(mods[index].hModule, filePath, MAX_PATH) == 0) {
            throw std::system_error(GetLastError(), std::system_category(), "Failed to get module filename");
        }
        
        std::string path = filePath;
        std::string name = mods[index].info.name;
        
        // Disable and unload the mod
        if (mods[index].enabled && mods[index].onDisable) {
            mods[index].onDisable();
        }
        
        FreeLibrary(mods[index].hModule);
        
        // Remove the mod from the vector
        mods.erase(mods.begin() + index);
        
        // Reload the mod
        std::cout << "Reloading mod: " << name << " from " << path << std::endl;
        return LoadModFromFile(path);
    } catch (const std::exception& e) {
        std::cout << "Error reloading mod at index " << index << ": " << e.what() << std::endl;
        return false;
    }
}
