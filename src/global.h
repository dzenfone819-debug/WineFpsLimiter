#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include "imgui.h"

// --- Shared Configuration ---
struct Config {
    bool EnableStartLimit = true;
    int TargetFPS = 60;
    
    bool EnableBackgroundLimit = true;
    int BackgroundFPS = 15;

    bool ShowMenu = true; 
    bool StartVisible = true;
    
    bool ShowClock = false;
    bool ShowSessionTime = false;
    bool ShowTotalTime = false;
    bool ShowRenderer = false;
    long long TotalPlaytime = 0; 

    int HotkeyMode = 0; 
};

// --- Shared Globals ---
extern Config g_Config;
extern bool g_ImGuiInitialized;
extern std::string g_RendererName;
extern std::chrono::steady_clock::time_point g_StartupTime;
extern std::chrono::steady_clock::time_point g_LastSaveTime;
extern std::chrono::high_resolution_clock::time_point g_lastFrameTime;
extern std::vector<float>* frame_times;
extern HWND g_hWnd;
extern WNDPROC g_oWndProc;

// --- Helper Functions ---
void Log(const char* fmt, ...);
void SaveConfig();
void LoadConfig();
HWND FindMainWindow();
LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// --- Backend Initialization Prototypes ---
void InitDX9Hook();
void InitDX12Hook();
void InitOpenGLHook();

// --- Common Overlay Logic ---
void RenderOverlay();
void CheckLimiter();
