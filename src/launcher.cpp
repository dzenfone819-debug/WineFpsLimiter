#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <shlwapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shlwapi.lib")

// --- Data Structures ---
struct GameProfile {
    char Name[64];
    char Path[MAX_PATH];
    
    bool EnableLimit = true;
    int TargetFPS = 60;
    bool EnableBgLimit = true;
    int BgFPS = 15;
    bool ShowClock = false;
    bool ShowTimer = false;
    
    // New Settings
    int HotkeyMode = 0; // 0=Shift+Tab, 1=Backspace, etc.
    bool StartVisible = true;
    
    // Time
    bool ShowTotalTime = false;
    bool ShowRenderer = false; // New
    long long TotalPlaytime = 0; // In seconds

    // Runtime
    ID3D11ShaderResourceView* IconSRV = nullptr;
};

std::vector<GameProfile> g_Games;
int g_SelectedGame = -1;
int g_GlobalHotkeyMode = 0; // Global default

static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static IDXGISwapChain*          g_pSwapChain = NULL;
static ID3D11RenderTargetView*  g_mainRenderTargetView = NULL;

// --- Icon Helpers ---
ID3D11ShaderResourceView* CreateTextureFromHICON(ID3D11Device* device, HICON hIcon) {
    if (!hIcon) return nullptr;
    ICONINFO ii;
    if (!GetIconInfo(hIcon, &ii)) return nullptr;
    BITMAP bm;
    if (!GetObject(ii.hbmColor, sizeof(bm), &bm)) {
        if(ii.hbmColor) DeleteObject(ii.hbmColor);
        if(ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }
    int w = bm.bmWidth;
    int h = bm.bmHeight;
    HDC hdc = GetDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; 
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> pixels(w * h);
    GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; 
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = pixels.data();
    subResource.SysMemPitch = w * 4;
    ID3D11Texture2D* pTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &subResource, &pTexture);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (SUCCEEDED(hr) && pTexture) {
        ID3D11ShaderResourceView* srv = nullptr;
        device->CreateShaderResourceView(pTexture, NULL, &srv);
        pTexture->Release();
        return srv;
    }
    return nullptr;
}

void LoadGameIcon(GameProfile& g) {
    if (g.IconSRV) return;
    if (!g_pd3dDevice) return;
    SHFILEINFOA shfi = {};
    if (SHGetFileInfoA(g.Path, 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_LARGEICON)) {
        g.IconSRV = CreateTextureFromHICON(g_pd3dDevice, shfi.hIcon);
        DestroyIcon(shfi.hIcon);
    }
}

void LoadAllIcons() {
    for (auto& g : g_Games) LoadGameIcon(g);
}

// --- Helper: File I/O ---
std::string GetDatPath() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string fullPath = path;
    size_t lastSlash = fullPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) fullPath = fullPath.substr(0, lastSlash + 1);
    return fullPath + "launcher_games.dat";
}

void SaveGames() {
    std::ofstream out(GetDatPath());
    if (!out.is_open()) return;
    out << g_Games.size() << "|" << g_GlobalHotkeyMode << "\n";
    for (const auto& g : g_Games) {
        out << g.Name << "|" << g.Path << "|" 
            << g.EnableLimit << "|" << g.TargetFPS << "|"
            << g.EnableBgLimit << "|" << g.BgFPS << "|"
            << g.ShowClock << "|" << g.ShowTimer << "|"
            << g.HotkeyMode << "|" << g.StartVisible << "|" << g.ShowRenderer << "|"
            << g.ShowTotalTime << "|" << g.TotalPlaytime << "\n";
    }
}

void LoadGames() {
    std::ifstream in(GetDatPath());
    if (!in.is_open()) return;
    g_Games.clear();
    int count = 0;
    
    std::string headerLine;
    if (std::getline(in, headerLine)) {
        std::stringstream ss(headerLine);
        ss >> count;
        if (ss.peek() == '|') {
            ss.ignore();
            ss >> g_GlobalHotkeyMode;
        }
    }

    std::string line;
    while (count > 0 && std::getline(in, line)) {
        if (line.empty()) continue;
        GameProfile g = {};
        // Default init
        g.EnableLimit = true; g.TargetFPS = 60;
        g.HotkeyMode = g_GlobalHotkeyMode; 
        
        std::stringstream ss(line);
        std::string segment;
        std::vector<std::string> seglist;
        while(std::getline(ss, segment, '|')) seglist.push_back(segment);

        if (seglist.size() >= 2) {
            strncpy(g.Name, seglist[0].c_str(), 63);
            strncpy(g.Path, seglist[1].c_str(), MAX_PATH-1);
            try {
                if (seglist.size() > 2) g.EnableLimit = std::stoi(seglist[2]);
                if (seglist.size() > 3) g.TargetFPS = std::stoi(seglist[3]);
                if (seglist.size() > 4) g.EnableBgLimit = std::stoi(seglist[4]);
                if (seglist.size() > 5) g.BgFPS = std::stoi(seglist[5]);
                if (seglist.size() > 6) g.ShowClock = std::stoi(seglist[6]);
                if (seglist.size() > 7) g.ShowTimer = std::stoi(seglist[7]);
                if (seglist.size() > 8) g.HotkeyMode = std::stoi(seglist[8]);
                if (seglist.size() > 9) g.StartVisible = std::stoi(seglist[9]);
                if (seglist.size() > 10) g.ShowRenderer = std::stoi(seglist[10]);
                if (seglist.size() > 11) g.ShowTotalTime = std::stoi(seglist[11]);
                if (seglist.size() > 12) g.TotalPlaytime = std::stoll(seglist[12]);
                g_Games.push_back(g);
                count--;
            } catch (...) {}
        }
    }
}

// --- Helper: Injection ---
bool CheckFileArch(const char* path, DWORD* type) {
    DWORD binaryType;
    if (GetBinaryTypeA(path, &binaryType)) {
        *type = binaryType;
        return true;
    }
    return false;
}

void Inject(const GameProfile& game, HWND hwndLauncher) {
    std::string gameDir = game.Path;
    size_t lastSlash = gameDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) gameDir = gameDir.substr(0, lastSlash + 1);
    
    std::string configPath = gameDir + "fps_config.ini";
    
    // SYNC Logic for Launch:
    // We want the game to start with the MAX(launcher_time, disk_time).
    // And we want to ensure the disk has that value.
    char buf[64];
    GetPrivateProfileStringA("Settings", "TotalPlaytime", "0", buf, 64, configPath.c_str());
    long long iniTime = std::atoll(buf);
    long long finalTime = (iniTime > game.TotalPlaytime) ? iniTime : game.TotalPlaytime;

    WritePrivateProfileStringA("Settings", "EnableLimit", std::to_string(game.EnableLimit).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "TargetFPS", std::to_string(game.TargetFPS).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "EnableBgLimit", std::to_string(game.EnableBgLimit).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "BackgroundFPS", std::to_string(game.BgFPS).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowClock", std::to_string(game.ShowClock).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowSessionTime", std::to_string(game.ShowTimer).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "HotkeyMode", std::to_string(game.HotkeyMode).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "StartVisible", std::to_string(game.StartVisible).c_str(), configPath.c_str());
    
    WritePrivateProfileStringA("Settings", "ShowRenderer", std::to_string(game.ShowRenderer).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "ShowTotalTime", std::to_string(game.ShowTotalTime).c_str(), configPath.c_str());
    WritePrivateProfileStringA("Settings", "TotalPlaytime", std::to_string(finalTime).c_str(), configPath.c_str());

    DWORD binType = 0;
    CheckFileArch(game.Path, &binType);
    
    char dllPath[MAX_PATH];
    GetModuleFileNameA(NULL, dllPath, MAX_PATH);
    std::string launcherDir = dllPath;
    lastSlash = launcherDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) launcherDir = launcherDir.substr(0, lastSlash + 1);
    std::string fullDllPath = launcherDir + "FPSLimiter.dll"; 
    
#ifdef _WIN64
    if (binType == SCS_32BIT_BINARY) MessageBoxA(0, "Warning: x64 Launcher -> x86 Game mismatch.", "Arch Mismatch", 0);
#else
    if (binType == SCS_64BIT_BINARY) MessageBoxA(0, "Warning: x86 Launcher -> x64 Game mismatch.", "Arch Mismatch", 0);
#endif

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    char cmdLine[MAX_PATH + 32];
    sprintf(cmdLine, "\"%s\"", game.Path);

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, gameDir.c_str(), &si, &pi)) {
        void* pLoadLibrary = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
        void* pRemotePath = VirtualAllocEx(pi.hProcess, NULL, fullDllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE);
        WriteProcessMemory(pi.hProcess, pRemotePath, fullDllPath.c_str(), fullDllPath.size() + 1, NULL);
        
        HANDLE hThread = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, pRemotePath, 0, NULL);
        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
            ResumeThread(pi.hThread);
            ShowWindow(hwndLauncher, SW_MINIMIZE);
        } else {
            MessageBoxA(0, "Injection Failed", "Error", 0);
             TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        MessageBoxA(0, "Failed to start process", "Error", 0);
    }
}

// --- UI Logic ---
void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main(int, char**) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("ImGui Example"), NULL };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("FPS Manager Launcher"), WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    LoadGames();
    LoadAllIcons();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    const char* hotkey_items[] = { "Shift+Tab", "Backspace", "Insert", "Home", "F12", "F11" };

    bool show_demo_window = true;
    ImVec4 clear_color = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        {
            ImGui::SetNextWindowPos(ImVec2(0,0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

            ImGui::Columns(2, "games_cols");

            // LEFT PANEL
            ImGui::Text("My Games");
            ImGui::SameLine();
            if (ImGui::Button("+ Add")) {
                OPENFILENAMEA ofn = {0};
                char szFile[260] = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile);
                ofn.lpstrFilter = "Executables\0*.exe\0All\0*.*\0";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) {
                    GameProfile newGame = {};
                    strncpy(newGame.Path, szFile, MAX_PATH-1);
                    std::string s = szFile;
                    size_t slash = s.find_last_of("\\/");
                    std::string name = (slash != std::string::npos) ? s.substr(slash+1) : s;
                    strncpy(newGame.Name, name.c_str(), 63);
                    
                    newGame.HotkeyMode = g_GlobalHotkeyMode; 
                    
                    LoadGameIcon(newGame);
                    g_Games.push_back(newGame);
                    SaveGames();
                    g_SelectedGame = g_Games.size() - 1;
                }
            }
            // Global Settings Helper
            if (ImGui::Button("Set Global Hotkey")) {
                ImGui::OpenPopup("GlobalHotkeyPopup");
            }
            if (ImGui::BeginPopup("GlobalHotkeyPopup")) {
                ImGui::Text("Default Hotkey for NEW games:");
                if (ImGui::Combo("##gcombo", &g_GlobalHotkeyMode, hotkey_items, IM_ARRAYSIZE(hotkey_items))) {
                     SaveGames(); // Save header
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();
            
            for (int i = 0; i < g_Games.size(); i++) {
                if (g_Games[i].IconSRV) {
                    ImGui::Image((void*)g_Games[i].IconSRV, ImVec2(16, 16));
                    ImGui::SameLine();
                } else {
                    ImGui::Dummy(ImVec2(16, 16));
                    ImGui::SameLine();
                }
                if (ImGui::Selectable(g_Games[i].Name, g_SelectedGame == i)) {
                    g_SelectedGame = i;
                }
            }

            ImGui::NextColumn();

            // RIGHT PANEL
            if (g_SelectedGame >= 0 && g_SelectedGame < g_Games.size()) {
                GameProfile& g = g_Games[g_SelectedGame];
                
                // --- SYNC Logic ---
                // Try to read TotalPlaytime from fps_config.ini in game folder
                {
                   std::string gameDir = g.Path;
                   size_t lastSlash = gameDir.find_last_of("\\/");
                   if (lastSlash != std::string::npos) gameDir = gameDir.substr(0, lastSlash + 1);
                   std::string configPath = gameDir + "fps_config.ini";
                   
                   // Check if file exists/writable by trying to read.
                   // Reading 0 doesn't hurt.
                   char buf[64];
                   GetPrivateProfileStringA("Settings", "TotalPlaytime", "0", buf, 64, configPath.c_str());
                   long long diskTime = std::atoll(buf);
                   if (diskTime > g.TotalPlaytime) {
                       g.TotalPlaytime = diskTime;
                       SaveGames();
                   }
                }
                // ------------------

                if (g.IconSRV) {
                    ImGui::Image((void*)g.IconSRV, ImVec2(32, 32));
                    ImGui::SameLine();
                }
                ImGui::Text("Settings: %s", g.Name);
                
                // Total Time Display
                int th = g.TotalPlaytime / 3600;
                int tm = (g.TotalPlaytime % 3600) / 60;
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1.0f), "(Total: %dh %dm)", th, tm);

                ImGui::Separator();
                
                ImGui::InputText("Display Name", g.Name, 64);
                ImGui::LabelText("Path", "%s", g.Path);
                
                ImGui::Spacing();
                ImGui::Text("FPS Control");
                ImGui::Checkbox("Enable Limit", &g.EnableLimit);
                ImGui::SliderInt("Target FPS", &g.TargetFPS, 5, 240);
                
                ImGui::Checkbox("Background Limit", &g.EnableBgLimit);
                ImGui::SliderInt("Bg FPS", &g.BgFPS, 1, 60);
                
                ImGui::Spacing();
                ImGui::Text("Overlay");
                ImGui::Combo("Toggle Key", &g.HotkeyMode, hotkey_items, IM_ARRAYSIZE(hotkey_items));
                ImGui::Checkbox("Start Visible", &g.StartVisible);
                ImGui::Checkbox("Show Clock", &g.ShowClock);
                ImGui::Checkbox("Show Renderer Info", &g.ShowRenderer);
                ImGui::Checkbox("Show Session Timer", &g.ShowTimer);
                ImGui::Checkbox("Show Total Time", &g.ShowTotalTime);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("SAVE PROFILE", ImVec2(120, 30))) SaveGames();
                ImGui::SameLine();
                if (ImGui::Button("REMOVE GAME", ImVec2(120, 30))) {
                    if (g_Games[g_SelectedGame].IconSRV) g_Games[g_SelectedGame].IconSRV->Release();
                    g_Games.erase(g_Games.begin() + g_SelectedGame);
                    g_SelectedGame = -1;
                    SaveGames();
                }

                ImGui::Spacing();
                ImGui::Spacing();
                
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.33f, 0.8f, 0.8f));
                if (ImGui::Button("LAUNCH GAME", ImVec2(-1, 50))) {
                    SaveGames(); 
                    Inject(g, hwnd);
                }
                ImGui::PopStyleColor(3);

            } else {
                ImGui::TextDisabled("Select a game to edit settings...");
            }

            ImGui::End();
        }
        
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    for(auto& g : g_Games) if(g.IconSRV) g.IconSRV->Release();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}
