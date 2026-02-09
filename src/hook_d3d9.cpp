#include "global.h"
#include <d3d9.h>
#include "MinHook.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

// --- Function Pointers ---
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
Direct3DCreate9_t g_oDirect3DCreate9 = nullptr;

typedef HRESULT(__stdcall *CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDevice_t g_oCreateDevice = nullptr;

typedef HRESULT(__stdcall *EndScene_t)(IDirect3DDevice9*);
EndScene_t g_oEndScene = nullptr;

typedef HRESULT(__stdcall *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
Reset_t g_oReset = nullptr;

// --- Implementations ---

HRESULT __stdcall hookReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_oReset(pDevice, pPresentationParameters);
    ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

HRESULT __stdcall hookEndScene(IDirect3DDevice9* pDevice) {
    static bool init = false;
    if (!init) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        pDevice->GetCreationParameters(&cp);
        g_hWnd = cp.hFocusWindow;
        if (!g_hWnd) g_hWnd = FindMainWindow();

        if (g_hWnd) {
             WNDPROC current = (WNDPROC)GetWindowLongPtr(g_hWnd, GWLP_WNDPROC);
             if (current != (WNDPROC)WndProc) {
                 g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
             }
        }

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(pDevice);
        
        g_RendererName = "DirectX 9";
        IDirect3D9* pD3D = nullptr;
        if(SUCCEEDED(pDevice->GetDirect3D(&pD3D))) {
            D3DADAPTER_IDENTIFIER9 id;
            if(SUCCEEDED(pD3D->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id))) {
                g_RendererName = id.Description;
            }
            pD3D->Release();
        }
        
        init = true;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    CheckLimiter();
    RenderOverlay();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return g_oEndScene(pDevice);
}

HRESULT __stdcall hookCreateDevice(IDirect3D9* pD3D, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) {
    HRESULT hr = g_oCreateDevice(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        IDirect3DDevice9* pDevice = *ppReturnedDeviceInterface;
        void** vTable = *reinterpret_cast<void***>(pDevice);
        
        // Hook EndScene (42) and Reset (16)
        if (g_oEndScene == nullptr) {
            MH_CreateHook(vTable[42], (LPVOID)hookEndScene, (LPVOID*)&g_oEndScene);
            MH_EnableHook(vTable[42]);
        }
        if (g_oReset == nullptr) {
            MH_CreateHook(vTable[16], (LPVOID)hookReset, (LPVOID*)&g_oReset);
            MH_EnableHook(vTable[16]);
        }
        Log("D3D9 Device Hooked via CreateDevice.");
    }
    return hr;
}

IDirect3D9* WINAPI hookDirect3DCreate9(UINT SDKVersion) {
    IDirect3D9* pD3D = g_oDirect3DCreate9(SDKVersion);
    if (pD3D) {
        // Hook CreateDevice (Index 16)
        void** vTable = *reinterpret_cast<void***>(pD3D);
        if (g_oCreateDevice == nullptr) {
             MH_CreateHook(vTable[16], (LPVOID)hookCreateDevice, (LPVOID*)&g_oCreateDevice);
             MH_EnableHook(vTable[16]);
             Log("D3D9 CreateDevice Hooked.");
        }
    }
    return pD3D;
}

void InitDX9Hook() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (hD3D9) {
        void* addr = (void*)GetProcAddress(hD3D9, "Direct3DCreate9");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hookDirect3DCreate9, (LPVOID*)&g_oDirect3DCreate9);
            MH_EnableHook(addr);
            Log("Direct3DCreate9 Export Hooked.");
        }
    }
}
