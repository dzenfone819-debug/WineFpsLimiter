#include "global.h"
#include <windows.h>
#include <cstdio>
#include <vector>
#include <thread>
#include <string>
#include <ctime>

#include "MinHook.h"
#include "imgui_impl_win32.h"

// --- Definitions of Globals ---
Config g_Config;
bool g_ImGuiInitialized = false;
std::string g_RendererName = "Unknown GPU";

std::chrono::steady_clock::time_point g_StartupTime;
std::chrono::steady_clock::time_point g_LastSaveTime;
std::chrono::high_resolution_clock::time_point g_lastFrameTime;
std::vector<float>* frame_times = nullptr;
float g_lastFrameTimeMs = 0.0f;
HWND g_hWnd = nullptr;
WNDPROC g_oWndProc = nullptr;
static FILE* logFile = nullptr;

// --- Helper Impl ---
void Log(const char* fmt, ...) {
    if (!logFile) {
        logFile = fopen("C:\\fps_limiter.log", "w");
        if (!logFile) logFile = fopen("fps_limiter.log", "w");
    }
    if (logFile) {
        va_list args;
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        vfprintf(logFile, "\n", nullptr);
        va_end(args);
        fflush(logFile);
    }
}

void SaveConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = std::string(path);
    size_t lastSlash = configPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) configPath = configPath.substr(0, lastSlash + 1);
    configPath += "fps_config.ini";

    auto now = std::chrono::steady_clock::now();
    long long sessionSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - g_StartupTime).count();
    long long currentTotal = g_Config.TotalPlaytime + sessionSeconds;

    WritePrivateProfileStringA("Settings", "EnableLimit", std::to_string(g_Config.EnableStartLimit).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "TargetFPS", std::to_string(g_Config.TargetFPS).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "EnableBgLimit", std::to_string(g_Config.EnableBackgroundLimit).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "BackgroundFPS", std::to_string(g_Config.BackgroundFPS).c_str(), configPath.c_str());
    
    WritePrivateProfileStringA("Settings", "ShowClock", std::to_string(g_Config.ShowClock).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowSessionTime", std::to_string(g_Config.ShowSessionTime).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowTotalTime", std::to_string(g_Config.ShowTotalTime).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowRenderer", std::to_string(g_Config.ShowRenderer).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowFPS", std::to_string(g_Config.ShowFPS).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowFrameTime", std::to_string(g_Config.ShowFrameTime).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowFrameGraph", std::to_string(g_Config.ShowFrameGraph).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "HotkeyMode", std::to_string(g_Config.HotkeyMode).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "TotalPlaytime", std::to_string(currentTotal).c_str(), configPath.c_str());
}

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = std::string(path);
    size_t lastSlash = configPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) configPath = configPath.substr(0, lastSlash + 1);
    configPath += "fps_config.ini";

    g_Config.EnableStartLimit = GetPrivateProfileIntA("Settings", "EnableLimit", 1, configPath.c_str());
    g_Config.TargetFPS = GetPrivateProfileIntA("Settings", "TargetFPS", 60, configPath.c_str());
    g_Config.EnableBackgroundLimit = GetPrivateProfileIntA("Settings", "EnableBgLimit", 1, configPath.c_str());
    g_Config.BackgroundFPS = GetPrivateProfileIntA("Settings", "BackgroundFPS", 15, configPath.c_str());
    
    g_Config.ShowClock = GetPrivateProfileIntA("Settings", "ShowClock", 0, configPath.c_str());
    g_Config.ShowSessionTime = GetPrivateProfileIntA("Settings", "ShowSessionTime", 0, configPath.c_str());
    g_Config.ShowTotalTime = GetPrivateProfileIntA("Settings", "ShowTotalTime", 0, configPath.c_str());
    g_Config.ShowRenderer = GetPrivateProfileIntA("Settings", "ShowRenderer", 0, configPath.c_str());
    g_Config.ShowFPS = GetPrivateProfileIntA("Settings", "ShowFPS", 1, configPath.c_str());
    g_Config.ShowFrameTime = GetPrivateProfileIntA("Settings", "ShowFrameTime", 0, configPath.c_str());
    g_Config.ShowFrameGraph = GetPrivateProfileIntA("Settings", "ShowFrameGraph", 0, configPath.c_str());
    g_Config.HotkeyMode = GetPrivateProfileIntA("Settings", "HotkeyMode", 0, configPath.c_str());
    
    char buf[32];
    GetPrivateProfileStringA("Settings", "TotalPlaytime", "0", buf, 32, configPath.c_str());
    g_Config.TotalPlaytime = strtoll(buf, NULL, 10);
}

// --- ImGui Logic ---

// Forward declare WndProc
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return true;
    
    // Hotkey Handling
    bool toggle = false;
    switch(g_Config.HotkeyMode) {
        case 0: if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && (uMsg == WM_KEYDOWN && wParam == VK_TAB)) toggle = true; break; // Shift+Tab
        case 1: if (uMsg == WM_KEYDOWN && wParam == VK_BACK) toggle = true; break; // Backspace
        case 2: if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) toggle = true; break; // Insert
        case 3: if (uMsg == WM_KEYDOWN && wParam == VK_HOME) toggle = true; break; // Home
        case 4: if (uMsg == WM_KEYDOWN && wParam == VK_F12) toggle = true; break; // F12
        case 5: if (uMsg == WM_KEYDOWN && wParam == VK_F11) toggle = true; break; // F11
    }

    if (toggle) {
        g_Config.ShowMenu = !g_Config.ShowMenu;
        SaveConfig();
    }

    if (g_oWndProc)
        return CallWindowProc(g_oWndProc, hWnd, uMsg, wParam, lParam);
    else
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

HWND FindMainWindow() {
    // Basic heuristic to find the game window
    return GetForegroundWindow();
}

void CheckLimiter() {
    auto now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> dt = now - g_lastFrameTime;
    g_lastFrameTime = now;

    if (frame_times) {
        frame_times->push_back(dt.count());
        if (frame_times->size() > 100) frame_times->erase(frame_times->begin());
    }

    g_lastFrameTimeMs = dt.count();

    int target = g_Config.TargetFPS;
    if (g_Config.EnableBackgroundLimit && GetForegroundWindow() != g_hWnd) {
        target = g_Config.BackgroundFPS;
    }

    if (g_Config.EnableStartLimit && target > 0) {
        double frameTimeMin = 1000.0 / target;
        std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - now;
        if (elapsed.count() < frameTimeMin) {
            DWORD sleepTime = (DWORD)(frameTimeMin - elapsed.count());
            if (sleepTime > 1) Sleep(sleepTime - 1);
            while ((std::chrono::high_resolution_clock::now() - now).count() < frameTimeMin) {
                // spin
            }
        }
    }
}

void RenderOverlay() {
    if (!frame_times) frame_times = new std::vector<float>();

    if (std::chrono::steady_clock::now() - g_LastSaveTime > std::chrono::seconds(30)) {
        SaveConfig();
        g_LastSaveTime = std::chrono::steady_clock::now();
    }

    if (g_Config.ShowMenu) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("FPS Control", &g_Config.ShowMenu, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Separator();
            if (ImGui::Checkbox("FPS Limit", &g_Config.EnableStartLimit)) SaveConfig();
            if (g_Config.EnableStartLimit) {
                if (ImGui::SliderInt("Target", &g_Config.TargetFPS, 5, 240)) SaveConfig();
            }
            if (ImGui::Checkbox("Bg Limit", &g_Config.EnableBackgroundLimit)) SaveConfig();
            if (g_Config.EnableBackgroundLimit) {
                if (ImGui::SliderInt("Bg FPS", &g_Config.BackgroundFPS, 1, 60)) SaveConfig();
            }
            ImGui::Separator();
            if (ImGui::Checkbox("Clock", &g_Config.ShowClock)) SaveConfig();
            if (ImGui::Checkbox("Timer", &g_Config.ShowSessionTime)) SaveConfig();
            if (ImGui::Checkbox("Total Time", &g_Config.ShowTotalTime)) SaveConfig();
            if (ImGui::Checkbox("Renderer", &g_Config.ShowRenderer)) SaveConfig();
            if (ImGui::Checkbox("Show FPS", &g_Config.ShowFPS)) SaveConfig();
            if (ImGui::Checkbox("Frame Time", &g_Config.ShowFrameTime)) SaveConfig();
            if (ImGui::Checkbox("Frame Graph", &g_Config.ShowFrameGraph)) SaveConfig();
            ImGui::Separator();
            const char* items[] = { "Shift+Tab", "Backspace", "Insert", "Home", "F12", "F11" };
            if (ImGui::Combo("Hotkey", &g_Config.HotkeyMode, items, IM_ARRAYSIZE(items))) SaveConfig();
        }
        ImGui::End();
    }

    if (g_Config.ShowClock || g_Config.ShowSessionTime || g_Config.ShowTotalTime || g_Config.ShowRenderer || g_Config.ShowFPS || g_Config.ShowFrameTime || g_Config.ShowFrameGraph) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.3f);
            ImGui::Begin("OverlayStats", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            
            if (g_Config.ShowRenderer) {
                ImGui::Text("%s", g_RendererName.c_str());
            }

            if (g_Config.ShowFPS) {
                ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            }

            if (g_Config.ShowFrameTime) {
                ImGui::Text("Frame Time: %.2f ms", g_lastFrameTimeMs);
            }

            if (g_Config.ShowClock) {
                time_t now = time(0);
                struct tm tstruct;
                char buf[80];
                tstruct = *localtime(&now);
                strftime(buf, sizeof(buf), "%X", &tstruct);
                ImGui::Text("TIME: %s", buf);
            }

            if (g_Config.ShowSessionTime) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_StartupTime).count();
                int h = elapsed / 3600;
                int m = (elapsed % 3600) / 60;
                int s = elapsed % 60;
                ImGui::Text("PLAY: %02d:%02d:%02d", h, m, s);
            }

            if (g_Config.ShowTotalTime) {
                auto now = std::chrono::steady_clock::now();
                long long session = std::chrono::duration_cast<std::chrono::seconds>(now - g_StartupTime).count();
                long long total = g_Config.TotalPlaytime + session;
                int h = total / 3600;
                int m = (total % 3600) / 60;
                ImGui::Text("TOTAL: %dh %dm", h, m);
            }

            if (g_Config.ShowFrameGraph && frame_times && !frame_times->empty()) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f)); // Dark background for graph
                ImGui::PlotLines("##FrameGraph", frame_times->data(), (int)frame_times->size(), 0, NULL, 0.0f, 33.0f, ImVec2(0, 50));
                ImGui::PopStyleColor();
            }

            ImGui::End();
    }
}

// Hooks to other modules
extern void InitDX9Hook();
extern void InitDXGIHook();
extern void InitOpenGLHook();

DWORD WINAPI MainThread(LPVOID lpReserved) {
    g_StartupTime = std::chrono::steady_clock::now();
    LoadConfig();
    Log("DLL Injected.");
    
    if (MH_Initialize() != MH_OK) return FALSE;

    // Retry loop for late loading modules
    for (int i = 0; i < 15; i++) {
        InitDX9Hook();
        InitDXGIHook();
        InitOpenGLHook();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Start a background thread to perform a delayed, safe scan for CommandQueue implementations.
    // This reduces risk of instability during early startup while still attempting to intercept the real queue.
    std::thread([](){
        std::this_thread::sleep_for(std::chrono::seconds(5)); // give process time to finish initialization
        for (int attempt = 0; attempt < 6; ++attempt) {
            Log("ScanThread: attempt %d to scan for CommandQueue hooks", attempt+1);
            extern bool ScanForCommandQueueHooks();
            if (ScanForCommandQueueHooks()) {
                Log("ScanThread: successfully installed ExecuteCommandLists hook");
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();

    return TRUE;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        SaveConfig();
        if (logFile) fclose(logFile);
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
