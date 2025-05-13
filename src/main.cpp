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
#include "include/api.h"
#include "include/layer.h"
#include "include/menu.hpp"
#include "include/mod_loader.h"

namespace {
    // Global variables
    HMODULE g_dllHandle = nullptr;
    std::string g_basePath;
    std::atomic<bool> g_isInitialized{ false };
    std::mutex g_logMutex;

    // Function pointers for original PowerProf functions
    BOOLEAN(*g_originalGetPwrCapabilities)(PSYSTEM_POWER_CAPABILITIES) = nullptr;
    NTSTATUS(*g_originalCallNtPowerInformation)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG) = nullptr;
    POWER_PLATFORM_ROLE(*g_originalPowerDeterminePlatformRole)() = nullptr;

    // Registry function hook
    typedef LSTATUS(__stdcall* PFN_RegEnumValueA)(HKEY hKey, DWORD dwIndex, LPSTR lpValueName, LPDWORD lpcchValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
    PFN_RegEnumValueA g_originalRegEnumValueA = nullptr;

    // Window procedure hook
    WNDPROC g_originalWndProc = nullptr;

    // Log file
    std::ofstream g_logFile;
}

// Custom Streambuf for logging to both file and console
class StreamBuf : public std::streambuf {
public:
    StreamBuf(std::streambuf* buf, const std::string& prefix)
        : m_originalBuf(buf), m_logPrefix(prefix) {
    }

protected:
    virtual int overflow(int c) override {
        if (c != EOF) {
            if (c == '\n') {
                std::lock_guard<std::mutex> lock(g_logMutex);
                g_logFile << m_logPrefix << m_buffer << std::endl;
                g_logFile.flush();
                m_buffer.clear();
            }
            else {
                m_buffer += static_cast<char>(c);
            }
        }
        return m_originalBuf->sputc(c);
    }

private:
    std::streambuf* m_originalBuf;
    std::string m_logPrefix;
    std::string m_buffer;
};

// Utility functions
void Print(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::cout << std::string(buffer);
}

void InitLogger() {
    g_logFile.open("SML.log", std::ios::out | std::ios::app);

    // Redirect std::cout, and std::cerr to SML.log
    static StreamBuf coutBuf(std::cout.rdbuf(), "[OUTPUT] ");
    std::cout.rdbuf(&coutBuf);

    static StreamBuf cerrBuf(std::cerr.rdbuf(), "[ERROR] ");
    std::cerr.rdbuf(&cerrBuf);
}

void InitConsole() {
    FreeConsole();
    AllocConsole();
    SetConsoleTitleA("SML Console");

    if (IsValidCodePage(CP_UTF8)) {
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
    }

    auto hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hStdout, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // Disable Ctrl+C handling
    SetConsoleCtrlHandler(NULL, TRUE);

    CONSOLE_FONT_INFOEX cfi = {};
    cfi.cbSize = sizeof(cfi);
    GetCurrentConsoleFontEx(hStdout, FALSE, &cfi);

    // Change to a more readable font if user has one of the default fonts
    if (wcscmp(cfi.FaceName, L"Terminal") == 0 ||
        wcscmp(cfi.FaceName, L"Courier New") == 0 ||
        (cfi.FontFamily & TMPF_VECTOR) == 0) {

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

std::wstring GetKeyPathFromKKEY(HKEY key) {
    std::wstring keyPath;
    if (key == NULL) return keyPath;

    HMODULE dll = LoadLibraryA("ntdll.dll");
    if (dll == NULL) return keyPath;

    typedef DWORD(__stdcall* NtQueryKeyType)(
        HANDLE KeyHandle,
        int KeyInformationClass,
        PVOID KeyInformation,
        ULONG Length,
        PULONG ResultLength);

    NtQueryKeyType func = reinterpret_cast<NtQueryKeyType>(GetProcAddress(dll, "NtQueryKey"));
    if (func == NULL) {
        FreeLibrary(dll);
        return keyPath;
    }

    const DWORD STATUS_SUCCESS = 0x00000000L;
    const DWORD STATUS_BUFFER_TOO_SMALL = 0xC0000023L;

    DWORD size = 0;
    DWORD result = func(key, 3, nullptr, 0, &size);

    if (result == STATUS_BUFFER_TOO_SMALL) {
        size += 2; // Extra space for null terminator
        auto buffer = std::make_unique<wchar_t[]>(size / sizeof(wchar_t));

        if (buffer) {
            result = func(key, 3, buffer.get(), size, &size);
            if (result == STATUS_SUCCESS) {
                buffer[size / sizeof(wchar_t)] = L'\0';
                keyPath = std::wstring(buffer.get() + 2); // Skip first two wchars
            }
        }
    }

    FreeLibrary(dll);
    return keyPath;
}

void TerminateCrashpadHandler() {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    if (Process32First(snapshot, &entry)) {
        do {
            if (lstrcmpA(entry.szExeFile, "crashpad_handler.exe") == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (hProcess) {
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                    Print("Detected and closed crashpad_handler.exe\n");
                }
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

// Hook functions
LRESULT WINAPI HookWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_HOME) {
        Menu::bShowMenu = !Menu::bShowMenu;
        return 0;
    }

    LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    if (Menu::bShowMenu) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return ERROR_SUCCESS;
        }

        // Handle mouse input when menu is active
        if (ImGui::GetIO().WantCaptureMouse &&
            (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP ||
                uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP ||
                uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP ||
                uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEMOVE)) {
            return ERROR_SUCCESS;
        }
    }

    return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
}

LSTATUS WINAPI HookRegEnumValueA(HKEY hKey, DWORD dwIndex, LPSTR lpValueName, LPDWORD lpcchValueName,
    LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {
    std::wstring path = GetKeyPathFromKKEY(hKey);

    LSTATUS result = g_originalRegEnumValueA(hKey, dwIndex, lpValueName, lpcchValueName, lpReserved, lpType, lpData, lpcbData);

    if (wcscmp(path.c_str(), L"\\REGISTRY\\MACHINE\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers") == 0 && dwIndex == 0) {
        std::string configPath = g_basePath + "\\sml_config.json";

        strncpy_s(lpValueName, *lpcchValueName, configPath.c_str(), configPath.size());
        lpValueName[configPath.size()] = '\0';

        *lpcchValueName = MAX_PATH;
        lpData = nullptr;
        *lpcbData = 4;
    }

    return result;
}

// PowerProf function exports
extern "C" __declspec(dllexport) BOOLEAN __stdcall GetPwrCapabilities(PSYSTEM_POWER_CAPABILITIES lpspc) {
    return g_originalGetPwrCapabilities(lpspc);
}

extern "C" __declspec(dllexport) NTSTATUS __stdcall CallNtPowerInformation(
    POWER_INFORMATION_LEVEL InformationLevel,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength) {
    return g_originalCallNtPowerInformation(InformationLevel, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);
}

extern "C" __declspec(dllexport) POWER_PLATFORM_ROLE PowerDeterminePlatformRole() {
    return g_originalPowerDeterminePlatformRole();
}

// Thread functions
DWORD WINAPI HookThreadProc(LPVOID) {
    HWND window = nullptr;
    Print("Searching for Sky Window...\n");

    // Wait for the Sky window to be created
    while (!window && !GetAsyncKeyState(VK_END)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        window = FindWindowA("TgcMainWindow", "Sky");
    }

    if (!window) {
        Print("Window search interrupted or failed\n");
        return EXIT_FAILURE;
    }

    // Set up the layer and window hook
    layer::setup(window);
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc))
        );

    return EXIT_SUCCESS;
}

bool LoadPowerProfFunctions() {
    // Try to load the original DLL
    g_dllHandle = LoadLibraryA("C:\\Windows\\System32\\powrprof.dll");
    if (!g_dllHandle) {
        g_dllHandle = LoadLibraryA("C:\\Windows\\System32\\POWRPROF.dll");
    }

    Print("Loading powrprof.dll symbols...\n");

    if (!g_dllHandle) {
        Print("Failed to load powrprof.dll\n");
        return false;
    }

    // Get the original function addresses
    g_originalGetPwrCapabilities = reinterpret_cast<decltype(g_originalGetPwrCapabilities)>(
        GetProcAddress(g_dllHandle, "GetPwrCapabilities")
        );

    g_originalCallNtPowerInformation = reinterpret_cast<decltype(g_originalCallNtPowerInformation)>(
        GetProcAddress(g_dllHandle, "CallNtPowerInformation")
        );

    g_originalPowerDeterminePlatformRole = reinterpret_cast<decltype(g_originalPowerDeterminePlatformRole)>(
        GetProcAddress(g_dllHandle, "PowerDeterminePlatformRole")
        );

    if (!g_originalGetPwrCapabilities || !g_originalCallNtPowerInformation || !g_originalPowerDeterminePlatformRole) {
        Print("Could not locate required symbols in powrprof.dll\n");
        return false;
    }

    return true;
}

bool SetupRegistryHook() {
    HMODULE handle = LoadLibraryA("advapi32.dll");
    if (!handle) {
        Print("Failed to load advapi32.dll\n");
        return false;
    }

    lm_address_t fnRegEnumValue = reinterpret_cast<lm_address_t>(
        GetProcAddress(handle, "RegEnumValueA")
        );

    if (!fnRegEnumValue) {
        Print("fnRegEnumValue address is null, possible corrupted file\n");
        return false;
    }

    if (!LM_HookCode(
        fnRegEnumValue,
        reinterpret_cast<lm_address_t>(&HookRegEnumValueA),
        reinterpret_cast<lm_address_t*>(&g_originalRegEnumValueA))) {
        Print("Failed to hook fnRegEnumValue\n");
        return false;
    }

    return true;
}

void Initialize() {
    // Prevent multiple initializations
    if (g_isInitialized.exchange(true)) {
        return;
    }

    // Set up console and logging
    InitConsole();
    std::remove("SML.log");
    InitLogger();

    // Get base path
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring ws(path);
    
    // Convert wide string to narrow string using Windows API for proper encoding
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string fullPath(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &fullPath[0], size_needed, NULL, NULL);
    fullPath.resize(strlen(fullPath.c_str())); // Adjust size to actual content
    
    g_basePath = fullPath.substr(0, fullPath.find_last_of("\\/"));

    // Load PowerProf functions
    if (!LoadPowerProfFunctions()) {
        Print("Failed to initialize PowerProf functions\n");
        return;
    }

    // Set up registry hook
    if (SetupRegistryHook()) {
        // Terminate crashpad handler
        TerminateCrashpadHandler();

        // Initialize mod system
        ModApi::Instance().InitSkyBase();
        ModLoader::LoadMods();

        // Start window hook thread
        HANDLE threadHandle = CreateThread(NULL, 0, HookThreadProc, nullptr, 0, NULL);
        if (threadHandle) {
            CloseHandle(threadHandle);
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    DisableThreadLibraryCalls(hinstDLL);

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        Initialize();
        break;
    case DLL_PROCESS_DETACH:
        if (g_dllHandle) {
            FreeLibrary(g_dllHandle);
        }
        break;
    }

    return TRUE;
}