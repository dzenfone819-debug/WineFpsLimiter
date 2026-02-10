#include "global.h"
#include <d3d11.h>
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// --- Globals for DX11 Backend ---
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_init11 = false;

void CleanupRenderTarget11() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void CreateRenderTarget11(IDXGISwapChain* pSwapChain) {
    ID3D11Texture2D* pBackBuffer;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void RenderDX11(IDXGISwapChain* pSwapChain) {
    if (!g_init11) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice))) return;
        g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
        
        DXGI_SWAP_CHAIN_DESC sd;
        pSwapChain->GetDesc(&sd);
        g_hWnd = sd.OutputWindow;
        if (!IsWindow(g_hWnd)) g_hWnd = FindMainWindow();

        if (IsWindow(g_hWnd)) {
            WNDPROC current = (WNDPROC)GetWindowLongPtr(g_hWnd, GWLP_WNDPROC);
            if (current != (WNDPROC)WndProc) {
                g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
            }
        }

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        
        CreateRenderTarget11(pSwapChain);
        
        g_RendererName = "DirectX 11";
        
        // Try to get more info
        IDXGIDevice * pDXGIDevice = nullptr;
        if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&pDXGIDevice))) {
            IDXGIAdapter * pDXGIAdapter = nullptr;
            if (SUCCEEDED(pDXGIDevice->GetAdapter(&pDXGIAdapter))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(pDXGIAdapter->GetDesc(&desc))) {
                     char mb[128];
                     wcstombs(mb, desc.Description, 128);
                     g_RendererName = std::string(mb);
                }
                pDXGIAdapter->Release();
            }
            pDXGIDevice->Release();
        }

        g_init11 = true;
    }

    if (g_init11) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        RenderOverlay();

        ImGui::Render();
        
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void CleanupDX11() {
    if (g_init11) {
        CleanupRenderTarget11();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_init11 = false;
    }
}
