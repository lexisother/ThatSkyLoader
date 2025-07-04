#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <format>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include "include/menu.hpp"
#include "include/mod_loader.h"
#include "include/json.hpp"

using json = nlohmann::json;

namespace ig = ImGui;

// Font configuration structure
struct FontConfig {
    std::string fontPath;
    float fontSize = 0.0f;
    unsigned int unicodeRangeStart = 0;
    unsigned int unicodeRangeEnd = 0;
} fontconfig;  // Global instance

std::vector<std::string> Server_Names;
std::vector<std::string> Server_Urls;
std::string Selected_Url;
bool Server_Urls_Initialized = false;

namespace Menu {

void ReadServerUrls(const std::string& filepath, std::vector<std::string>& names, std::vector<std::string>& urls) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open config file: " + filepath);
        }

        json j;
        file >> j;

        // Access the Server_Urls object and populate names and values vectors
        for (const auto& item : j["Server_Urls"].items()) {
            names.push_back(item.key());
            urls.push_back(item.value().get<std::string>());
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error reading server URLs: " << e.what() << std::endl;
    }
}

std::string ReadDefaultServerUrl(const std::string& filepath) {
    std::string line;
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open AppInfo file: " + filepath);
        }

        for (int i = 0; i < 2; ++i) { // Read the second line
            std::getline(file, line);
            if (file.eof()) {
                throw std::runtime_error("File does not have enough lines: " + filepath);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error reading default server URL: " << e.what() << std::endl;
    }
    return line;
}

void ShowServerUrlSelector(const std::vector<std::string>& names, const std::vector<std::string>& urls, std::string& selectedurl) {
    if (ImGui::BeginCombo("Server", selectedurl.c_str())) {
        for (size_t i = 0; i < names.size(); ++i) {
            bool isSelected = (urls[i] == selectedurl);
            if (ImGui::Selectable(names[i].c_str(), isSelected)) {
                selectedurl = urls[i];
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void SaveSelectedServerUrl(const std::string& filepath, const std::string& selectedUrl) {
    try {
        std::ifstream infile(filepath);
        if (!infile.is_open()) {
            throw std::runtime_error("Could not open AppInfo file for reading: " + filepath);
        }

        std::string content;
        std::string line;
        int lineCount = 0;

        while (std::getline(infile, line)) {
            // Remove any trailing '\r' characters if there is any
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (lineCount == 1) { // Replace the second line
                content += selectedUrl + "\n";
            }
            else {
                content += line + "\n";
            }
            ++lineCount;
        }
        infile.close();

        std::ofstream outfile(filepath, std::ios::out | std::ios::binary);
        if (!outfile.is_open()) {
            throw std::runtime_error("Could not open AppInfo file for writing: " + filepath);
        }

        outfile << content;
    }
    catch (const std::exception& e) {
        std::cerr << "Error saving selected server URL: " << e.what() << std::endl;
    }
}

/**
 * @brief Load font configuration from a JSON file
 * @param filename Path to the JSON configuration file
 * @param fontconfig Font configuration to populate
 */
void loadFontConfig(const std::string& filename, FontConfig& fontconfig) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open " << filename << std::endl;
        return;
    }

    json jsonData;
    try {
        file >> jsonData;

        // Read fontPath
        fontconfig.fontPath = jsonData["fontPath"].get<std::string>();

        // Read fontSize
        fontconfig.fontSize = jsonData["fontSize"].get<float>();

        // Read unicodeRangeStart and unicodeRangeEnd
        fontconfig.unicodeRangeStart = static_cast<ImWchar>(std::stoi(jsonData["unicodeRangeStart"].get<std::string>(), nullptr, 16));
        fontconfig.unicodeRangeEnd = static_cast<ImWchar>(std::stoi(jsonData["unicodeRangeEnd"].get<std::string>(), nullptr, 16));

    } catch (const json::exception& e) {
        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
    }
    file.close();
}

/**
 * @brief Load fonts from the configured directory
 * @param fontconfig Font configuration to use
 */
void LoadFontsFromFolder(FontConfig& fontconfig) {
    // Set up Unicode range for font loading
    static const ImWchar ranges[] = {
        static_cast<ImWchar>(fontconfig.unicodeRangeStart), 
        static_cast<ImWchar>(fontconfig.unicodeRangeEnd),
        0  // end of list
    };
    
    ImGuiIO& io = ImGui::GetIO();
    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(fontconfig.fontPath)) {
            if (!entry.is_regular_file())
                continue;

            std::string filename = entry.path().string();
            if (fs::path(filename).extension() == ".ttf" || fs::path(filename).extension() == ".otf") {
                // Configure font loading parameters
                ImFontConfig fontCfg;
                fontCfg.OversampleH = 3;
                fontCfg.OversampleV = 3;
                fontCfg.PixelSnapH = true;

                // Attempt to load font
                if (!io.Fonts->AddFontFromFileTTF(filename.c_str(), fontconfig.fontSize, &fontCfg, ranges)) {
                    std::cerr << "Failed to load font: " << filename << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception while loading fonts: " << e.what() << std::endl;
    }
}

/**
 * @brief Display a font selector dropdown in the UI
 */
void ShowFontSelector() {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* currentFont = ImGui::GetFont();
    
    if (ImGui::BeginCombo("Font", currentFont->GetDebugName())) {
        for (ImFont* font : io.Fonts->Fonts) {
            const bool isSelected = (font == currentFont);
            
            ImGui::PushID(static_cast<void*>(font));
            if (ImGui::Selectable(font->GetDebugName(), isSelected)) {
                io.FontDefault = font;  // Set as default font
            }
            
            // Set initial focus when opening the combo
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
            
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

/**
 * @brief Initialize ImGui context and setup
 * @param hwnd Window handle to attach ImGui to
 */
void InitializeContext(HWND hwnd) {
    // Prevent double initialization
    if (ig::GetCurrentContext()) {
        return;
    }

    // Create ImGui context
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd);

    // Apply styling
    ImGuiStyle* style = &ImGui::GetStyle();
    style->WindowPadding = ImVec2(9, 9);
    style->FramePadding = ImVec2(9, 4);
    style->ItemSpacing = ImVec2(6, 4);
    style->ItemInnerSpacing = ImVec2(4, 4);
    style->IndentSpacing = 20;
    style->ScrollbarSize = 8;
    style->ScrollbarRounding = 12;
    style->GrabMinSize = 15;
    style->WindowBorderSize = 1;
    style->ChildBorderSize = 1;
    style->PopupBorderSize = 1;
    style->FrameBorderSize = 0;
    style->TabBorderSize = 1;
    style->TabBarBorderSize = 1;
    style->WindowRounding = 6;
    style->ChildRounding = 6;
    style->FrameRounding = 3;
    style->PopupRounding = 6;
    style->GrabRounding = 4;
    style->TabRounding = 4;
    style->CellPadding = ImVec2(2, 2);
    style->WindowTitleAlign = ImVec2(0.02f, 0.50f);
    style->SeparatorTextBorderSize = 2;
    style->SeparatorTextPadding = ImVec2(8, 0);

    ImVec4* colors = style->Colors;
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.21f, 0.21f, 0.21f, 0.18f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.73f);
    colors[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 0.50f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.18f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.29f, 0.29f, 0.29f, 0.40f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.53f, 0.53f, 0.53f, 0.67f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.54f, 0.54f, 0.54f, 0.31f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.69f, 0.69f, 0.69f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.83f, 0.83f, 0.83f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.29f, 0.29f, 0.29f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.29f, 0.29f, 0.29f, 0.50f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.29f, 0.29f, 0.29f, 0.50f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.52f, 0.52f, 0.52f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.67f, 0.67f, 0.67f, 0.50f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.74f, 0.74f, 0.74f, 0.95f);
    colors[ImGuiCol_Tab] = ImVec4(0.19f, 0.19f, 0.19f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.27f, 0.27f, 0.27f, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(0.46f, 0.46f, 0.46f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.27f, 0.27f, 0.27f, 0.80f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.37f, 0.37f, 0.37f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.46f, 0.46f, 0.46f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.77f, 0.77f, 0.77f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.91f, 0.91f, 0.91f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.49f, 0.49f, 0.49f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.72f, 0.72f, 0.72f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.66f, 0.66f, 0.66f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.63f, 0.63f, 0.63f, 1.00f);

    // Load font configuration
    loadFontConfig("tsml_config.json", fontconfig);
    LoadFontsFromFolder(fontconfig);

    // Configure ImGui IO
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;  // Disable .ini file saving
}

/**
 * @brief Display a help marker with tooltip
 * @param description Text to display in the tooltip
 */
void HelpMarker(const char* description) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/**
 * @brief Display the main SML menu with mod list and settings
 */
void SMLMainMenu() {
    char buf[64];
    ImGuiIO& io = ImGui::GetIO();

    ig::SetNextWindowSize({ 200, 0 }, ImGuiCond_Once);
    if (ig::Begin("That Sky Mod Loader", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SeparatorText(("Mods (" + std::to_string(ModLoader::GetModCount()) + ")").c_str());
        ig::BeginTable("##mods", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoBordersInBody);
        ig::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
        ig::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Info").x);

        for (int i = 0; i < ModLoader::GetModCount(); i++) {
            snprintf(buf, 64, "%s##check%d", ModLoader::GetModName(i).data(), i);
            ig::TableNextColumn();
            if (ig::Checkbox(buf, &ModLoader::GetModEnabled(i))) {
                if (ModLoader::GetModEnabled(i)) {
                    ModLoader::EnableMod(i);
                }
                else {
                    ModLoader::DisableMod(i);
                }
            }
            ig::TableNextColumn();
            HelpMarker(ModLoader::toString(i).c_str());
        }
        ig::EndTable();
        ig::SeparatorText("Settings");

        ShowFontSelector();
        ig::SameLine();
        HelpMarker(std::format("Total: {}\nPath: {}\nStart Range: {}\nEnd Range: {}\nSize: {}W / {}H\nConfig: tsml_config.json", 
            io.Fonts->Fonts.Size, fontconfig.fontPath.c_str(), fontconfig.unicodeRangeStart, 
            fontconfig.unicodeRangeEnd, io.Fonts->TexWidth, io.Fonts->TexHeight).c_str());

        const float MIN_SCALE = 0.3f;
        const float MAX_SCALE = 3.0f;
        static float window_scale = 1.0f;
        if (ig::DragFloat("Window Scale", &window_scale, 0.005f, MIN_SCALE, MAX_SCALE, "%.2f", ImGuiSliderFlags_AlwaysClamp))
            ig::SetWindowFontScale(window_scale);
        ig::DragFloat("Global Scale", &io.FontGlobalScale, 0.005f, MIN_SCALE, MAX_SCALE, "%.2f", ImGuiSliderFlags_AlwaysClamp);

        ig::Spacing();

        ig::PushStyleVar(ImGuiStyleVar_SeparatorTextBorderSize, 1.f);
        ig::SeparatorText("Custom Server");
        ig::PopStyleVar();

        if (!Server_Urls_Initialized) {
            ReadServerUrls("tsml_config.json", Server_Names, Server_Urls);
            Selected_Url = ReadDefaultServerUrl("data/AppInfo.tgc");
            Server_Urls_Initialized = true;
        }
        ShowServerUrlSelector(Server_Names, Server_Urls, Selected_Url);
        ig::SameLine();

        static bool Save_Server_URL = false;
        if (ig::Button("Save")) {
            Save_Server_URL = true;
            ig::OpenPopup("Confirmation");
        }

        if (Save_Server_URL) {
            ig::SetNextWindowSize(ImVec2(365, 120));
            if (ig::BeginPopupModal("Confirmation", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
                ig::TextWrapped("Are you sure you want to connect to this server? Your game will close to save changes.");
                ig::Spacing();

                if (ig::Button("Yes", ImVec2(120, 30))) {
                    SaveSelectedServerUrl("data/AppInfo.tgc", Selected_Url);
                    exit(0);
                    ig::CloseCurrentPopup();
                }
                ig::SameLine();
                if (ig::Button("No", ImVec2(120, 30))) {
                    ig::CloseCurrentPopup();
                }
                ig::EndPopup();
            }
        }

        ig::Spacing();
        ig::Separator();
        ig::Spacing();

        ig::Text("v0.2.0 | FPS: %.f | %.2f ms", io.Framerate, 1000.0f / io.Framerate);
    }
    ig::End();
}

/**
 * @brief Render the menu if enabled
 */
void Render() {
    if (!bShowMenu)
        return;

    SMLMainMenu();
    ModLoader::RenderAll();
}
}