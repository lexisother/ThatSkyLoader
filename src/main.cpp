#include <chrono>
#include <string>
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include <thread>
#include <dxgi.h>
#include <libmem.h>
#include <imgui.h>
#include <winreg.h>
#include <winuser.h>
#include <iostream>
#include <fstream>
#include <streambuf>
#include <mutex>
#include <atomic>
#include <tlhelp32.h>
#include <random>
#include <sstream>
#include <shellapi.h>
#include <psapi.h>
#include <vector>
#include <iomanip>
#include "include/api.h"
#include "include/layer.h"
#include "include/menu.hpp"
#include "include/mod_loader.h"
#include "include/json.hpp"


HMODULE dllHandle = nullptr;

BOOLEAN(*o_GetPwrCapabilities)(PSYSTEM_POWER_CAPABILITIES);
NTSTATUS(*o_CallNtPowerInformation)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);
POWER_PLATFORM_ROLE(*o_PowerDeterminePlatformRole)();

extern "C"
__declspec(dllexport) BOOLEAN __stdcall GetPwrCapabilities(PSYSTEM_POWER_CAPABILITIES lpspc) {
    return o_GetPwrCapabilities(lpspc);
}

extern "C"
__declspec(dllexport) NTSTATUS __stdcall CallNtPowerInformation(POWER_INFORMATION_LEVEL InformationLevel, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength) {
    return o_CallNtPowerInformation(InformationLevel, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);
}

extern "C"
__declspec(dllexport) POWER_PLATFORM_ROLE PowerDeterminePlatformRole() {
    return o_PowerDeterminePlatformRole();
}

std::string g_basePath;
// SML Log file
std::ofstream SML_Log;

// Custom Streambuf for logging to both file and console
class StreamBuf : public std::streambuf {
public:
    StreamBuf(std::streambuf* buf, const std::string& prefix)
        : originalBuf(buf), logPrefix(prefix) {
        // Reserve space to reduce reallocations
        buffer.reserve(256);
    }

protected:
    virtual int overflow(int c) override {
        if (c != EOF) {
            if (c == '\n') {
                // Write to log file with prefix
                SML_Log << logPrefix << buffer << std::endl;
                // Ensure data is written immediately
                SML_Log.flush();
                // Clear buffer efficiently (maintains capacity)
                buffer.clear();
            }
            else {
                // Append character to buffer
                buffer += static_cast<char>(c);
            }
        }
        // Always forward to original buffer
        return originalBuf->sputc(c);
    }

    // Implement sync for better control of buffer flushing
    virtual int sync() override {
        if (!buffer.empty()) {
            SML_Log << logPrefix << buffer << std::flush;
            buffer.clear();
        }
        return originalBuf->pubsync();
    }

private:
    std::streambuf* originalBuf;
    std::string logPrefix;
    std::string buffer;
};

void print(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::cout << std::string(buffer);
}

void InitLogger() {
    SML_Log.open("TSML.log", std::ios::out | std::ios::app);

    // Redirect std::cout, and std::cerr to SML.log
    static StreamBuf coutBuf(std::cout.rdbuf(), "[OUTPUT] ");
    std::cout.rdbuf(&coutBuf);

    static StreamBuf cerrBuf(std::cerr.rdbuf(), "[ERROR] ");
    std::cerr.rdbuf(&cerrBuf);
}

void InitConsole() {
    FreeConsole();
    AllocConsole();
    SetConsoleTitle("TSML Console");

    if (IsValidCodePage(CP_UTF8)) {
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
    }

    auto hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hStdout, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // Disable Ctrl+C handling
    SetConsoleCtrlHandler(NULL, TRUE);

    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    GetCurrentConsoleFontEx(hStdout, FALSE, &cfi);

    // Change to a more readable font if user has one of the default eyesore fonts
    if (wcscmp(cfi.FaceName, L"Terminal") == 0 || wcscmp(cfi.FaceName, L"Courier New") || (cfi.FontFamily & TMPF_VECTOR) == 0) {
        cfi.cbSize = sizeof(cfi);
        cfi.nFont = 0;
        cfi.dwFontSize.X = 0;
        cfi.dwFontSize.Y = 14;
        cfi.FontFamily = FF_MODERN | TMPF_VECTOR | TMPF_TRUETYPE;
        cfi.FontWeight = FW_NORMAL;
        wcscpy_s(cfi.FaceName, L"Lucida Console");
        SetCurrentConsoleFontEx(hStdout, FALSE, &cfi);
    }

    FILE* file;
    freopen_s(&file, "CONOUT$", "w", stdout);
    freopen_s(&file, "CONOUT$", "w", stderr);
    freopen_s(&file, "CONIN$", "r", stdin);

    fflush(stdout);
    fflush(stderr);
}

/**
 * @brief Ensures the SML config file exists, creating it with default values if it doesn't
 */
void EnsureConfigFileExists() {
    std::string configPath = g_basePath + "\\tsml_config.json";
    std::ifstream configFile(configPath);

    if (!configFile.is_open()) {
        print("Config file not found, creating default at: %s\n", configPath.c_str());

        // Write the default config directly to file to preserve exact order
        std::ofstream outFile(configPath);
        if (outFile.is_open()) {
            // Write the JSON with the exact formatting and order we want
            outFile << R"({
    "file_format_version" : "1.0.0",
    "layer" : {
      "name": "VkLayer_TSML",
      "type": "GLOBAL",
      "api_version": "1.3.221",
      "library_path": ".\\powrprof.dll",
      "implementation_version": "1",
      "description": "A mod loader for the game Sky: Chilren of the Light",
      "functions": {
        "vkGetInstanceProcAddr": "ModLoader_GetInstanceProcAddr",
        "vkGetDeviceProcAddr": "ModLoader_GetDeviceProcAddr"
      },
      "disable_environment": {
        "DISABLE_VKROOTS_TEST_1": "1"
      }
    },
    "fontPath": "fonts",
    "fontSize": 18.0,
    "unicodeRangeStart": "0x0001",
    "unicodeRangeEnd": "0xFFFF"
})";
            outFile.close();
            print("Created default config file successfully\n");
        }
        else {
            print("Failed to create default config file!\n");
        }
    }
    else {
        configFile.close();
    }
}

static WNDPROC oWndProc;
LRESULT WINAPI HookWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == 0xDF) {
        Menu::bShowMenu = !Menu::bShowMenu;
        std::cout << "ImGui menu toggled: " << (Menu::bShowMenu ? "Visible" : "Hidden") << std::endl;
        return 0;
    }

    try {
        LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return ERROR_SUCCESS;
        }

        // Handle mouse input when any ImGui window wants it
        if (ImGui::GetIO().WantCaptureMouse &&
            (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP ||
                uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP ||
                uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP ||
                uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEMOVE)) {
            return ERROR_SUCCESS;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in ImGui window procedure handler: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "Unknown exception in ImGui window procedure handler" << std::endl;
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

void terminateCrashpadHandler() {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (Process32First(snapshot, &entry) == TRUE) {
        while (Process32Next(snapshot, &entry) == TRUE) {
            if (lstrcmp(entry.szExeFile, "crashpad_handler.exe") == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);

                if (hProcess != NULL) {
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                    print("Detected and closed crashpad_handler.exe");
                }
            }
        }
    }
    CloseHandle(snapshot);
}

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif

std::wstring GetKeyPathFromKKEY(HKEY key)
{
    std::wstring keyPath;
    if (key != NULL)
    {
        HMODULE dll = LoadLibrary("ntdll.dll");
        if (dll != NULL) {
            typedef DWORD(__stdcall* NtQueryKeyType)(
                HANDLE  KeyHandle,
                int KeyInformationClass,
                PVOID  KeyInformation,
                ULONG  Length,
                PULONG  ResultLength);

            NtQueryKeyType func = reinterpret_cast<NtQueryKeyType>(::GetProcAddress(dll, "NtQueryKey"));

            if (func != NULL) {
                DWORD size = 0;
                DWORD result = 0;
                result = func(key, 3, 0, 0, &size);
                if (result == STATUS_BUFFER_TOO_SMALL)
                {
                    size = size + 2;
                    wchar_t* buffer = new (std::nothrow) wchar_t[size / sizeof(wchar_t)]; // size is in bytes
                    if (buffer != NULL)
                    {
                        result = func(key, 3, buffer, size, &size);
                        if (result == STATUS_SUCCESS)
                        {
                            buffer[size / sizeof(wchar_t)] = L'\0';
                            keyPath = std::wstring(buffer + 2);
                        }

                        delete[] buffer;
                    }
                }
            }

            FreeLibrary(dll);
        }
    }
    return keyPath;
}

#undef STATUS_BUFFER_TOO_SMALL
#undef STATUS_SUCCESS

typedef LSTATUS(__stdcall* PFN_RegEnumValueA)(HKEY hKey, DWORD dwIndex, LPSTR lpValueName, LPDWORD lpcchValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
PFN_RegEnumValueA oRegEnumValueA;
LSTATUS hkRegEnumValueA(HKEY hKey, DWORD dwIndex, LPSTR lpValueName, LPDWORD lpcchValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {

    std::wstring path = GetKeyPathFromKKEY(hKey);
    
    std::string name = g_basePath + "\\tsml_config.json";
    std::ifstream file(name);

    if (!file.is_open()) {
        EnsureConfigFileExists();
    }

    LSTATUS result = oRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, lpType, lpData, lpcbData);
    if (wcscmp(path.c_str(), L"\\REGISTRY\\MACHINE\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers") == 0 && dwIndex == 0) {

        for (size_t i = 0; i < name.size(); i++) {
            lpValueName[i] = name[i];
        }
        lpValueName[name.size()] = '\0';

        *lpcchValueName = 2048; // Max Path Length
        lpData = nullptr;
        *lpcbData = 4;
    }
    return result;
}

DWORD WINAPI hook_thread(PVOID lParam) {
    HWND window = nullptr;
    print("Searching for Sky Window\n");
    while (!window) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        window = FindWindowA("TgcMainWindow", "Sky");
    }
    layer::setup(window);
    oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
    return EXIT_SUCCESS;
}

void onAttach() {
    //loadWrapper(); - Integrated into onAttach()
    dllHandle = LoadLibrary("C:\\Windows\\System32\\powrprof.dll");

    if (dllHandle == NULL) dllHandle = LoadLibrary("C:\\Windows\\System32\\POWRPROF.dll");
    print("Loading powrprof.dll symbols...");

    if (dllHandle != NULL) {
        o_GetPwrCapabilities = (BOOLEAN(*)(PSYSTEM_POWER_CAPABILITIES))GetProcAddress(dllHandle, "GetPwrCapabilities");
        o_CallNtPowerInformation = (NTSTATUS(*)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG))GetProcAddress(dllHandle, "CallNtPowerInformation");
        o_PowerDeterminePlatformRole = (POWER_PLATFORM_ROLE(*)())GetProcAddress(dllHandle, "PowerDeterminePlatformRole");

        if (o_GetPwrCapabilities == nullptr || o_CallNtPowerInformation == nullptr || o_PowerDeterminePlatformRole == nullptr) {
            print("Could not locate symbols in powrprof.dll");
        }
    }
    else print("failed to load POWRPROF.dll");

    InitConsole();
    std::remove("TSML.log");
    InitLogger();
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring ws(path);
    std::string _path(ws.begin(), ws.end());
    g_basePath = _path.substr(0, _path.find_last_of("\\/"));

    HMODULE handle = LoadLibrary("advapi32.dll");
    if (handle != NULL) {
        lm_address_t fnRegEnumValue = (lm_address_t)GetProcAddress(handle, "RegEnumValueA");
        if (fnRegEnumValue == NULL) { std::cerr << "fnRegEnumValue address is null, possible corrupted file" << std::endl; return; } // this usually never happens, but still check just in case

        if (LM_HookCode(fnRegEnumValue, (lm_address_t)&hkRegEnumValueA, (lm_address_t*)&oRegEnumValueA)) {
            terminateCrashpadHandler();
            ModApi::Instance().InitSkyBase();
            ModLoader::LoadMods();
        }
        else print("Failed to hook fnRegEnumValue");

        CreateThread(NULL, 0, hook_thread, nullptr, 0, NULL);
    }
    else print("Failed to load advapi32.dll");   
}


BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    DisableThreadLibraryCalls(hinstDLL);

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        onAttach();
        break;
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}