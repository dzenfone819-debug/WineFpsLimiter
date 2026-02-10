#include "global.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "MinHook.h"

// --- External Backend Functions ---
extern void RenderDX11(IDXGISwapChain* pSwapChain);
extern void RenderDX12(IDXGISwapChain* pSwapChain);
extern void CleanupDX11();
extern void CleanupDX12();

// --- Globals ---
ID3D12CommandQueue* g_CapturedCommandQueue = nullptr;

// --- Forward Declarations ---
void InstallSwapChainHooks(IDXGISwapChain* pSwapChain);

// --- Function Pointers ---
typedef HRESULT(__stdcall *CreateDXGIFactory_t)(REFIID, void**);
CreateDXGIFactory_t g_fpCreateDXGIFactory = nullptr;
CreateDXGIFactory_t g_fpCreateDXGIFactory1 = nullptr;
typedef HRESULT(__stdcall *CreateDXGIFactory2_t)(UINT, REFIID, void**);
CreateDXGIFactory2_t g_fpCreateDXGIFactory2 = nullptr;

typedef HRESULT(__stdcall *CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
CreateSwapChain_t g_fpCreateSwapChain = nullptr;

typedef HRESULT(__stdcall *CreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainForHwnd_t g_fpCreateSwapChainForHwnd = nullptr;

typedef HRESULT(__stdcall *Present_t)(IDXGISwapChain*, UINT, UINT);
Present_t g_fpPresent = nullptr;

typedef HRESULT(__stdcall *ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ResizeBuffers_t g_fpResizeBuffers = nullptr;

// D3D12 Specific Hooks
typedef HRESULT(__stdcall *D3D12CreateDevice_t)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
D3D12CreateDevice_t g_fpD3D12CreateDevice = nullptr;

// D3D11 Specific Hooks
typedef HRESULT(__stdcall *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDeviceAndSwapChain_t g_fpD3D11CreateDeviceAndSwapChain = nullptr;

typedef HRESULT(__stdcall *CreateCommandQueue_t)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
CreateCommandQueue_t g_fpCreateCommandQueue = nullptr;


typedef void(__stdcall *ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteCommandLists_t g_fpExecuteCommandLists = nullptr;


// --- Hook Implementations ---

void __stdcall hookExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (pQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_CapturedCommandQueue = pQueue;
    }
    g_fpExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

HRESULT __stdcall hookCreateCommandQueue(ID3D12Device* pDevice, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
    HRESULT hr = g_fpCreateCommandQueue(pDevice, pDesc, riid, ppCommandQueue);
    if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue) {
        if (pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
             ID3D12CommandQueue* queue = (ID3D12CommandQueue*)*ppCommandQueue;
             void** vtable = *reinterpret_cast<void***>(queue);
             if (g_fpExecuteCommandLists == nullptr) {
                MH_CreateHook(vtable[54], (LPVOID)hookExecuteCommandLists, (LPVOID*)&g_fpExecuteCommandLists);
                MH_EnableHook(vtable[54]);
                Log("DX12 CommandQueue Intercepted.");
             }
        }
    }
    return hr;
}

HRESULT __stdcall hookD3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    HRESULT hr = g_fpD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ID3D12Device* device = (ID3D12Device*)*ppDevice;
        void** vtable = *reinterpret_cast<void***>(device);
        // CreateCommandQueue is index 9
        if (g_fpCreateCommandQueue == nullptr) {
            MH_CreateHook(vtable[9], (LPVOID)hookCreateCommandQueue, (LPVOID*)&g_fpCreateCommandQueue);
            MH_EnableHook(vtable[9]);
            Log("D3D12 Device Created & Hooked.");
        }
    }
    return hr;
}

HRESULT __stdcall hookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
{
    HRESULT hr = g_fpD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        InstallSwapChainHooks(*ppSwapChain);
        Log("D3D11 Device & SwapChain Created via Export.");
    }
    return hr;
}

HRESULT __stdcall hookResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    CleanupDX11();
    CleanupDX12(); // Simplistic: just nuke everything on resize
    return g_fpResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT __stdcall hookPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Detect Backend
    static int backendType = 0; // 0=Unk, 1=DX11, 2=DX12
    
    if (backendType == 0) {
        ID3D11Device* d3d11 = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11))) {
            backendType = 1;
            d3d11->Release();
        } else {
            ID3D12Device* d3d12 = nullptr;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12))) {
                backendType = 2;
                d3d12->Release();
            }
        }
    }

    CheckLimiter();

    if (backendType == 1) RenderDX11(pSwapChain);
    else if (backendType == 2) RenderDX12(pSwapChain);

    return g_fpPresent(pSwapChain, SyncInterval, Flags);
}

void InstallSwapChainHooks(IDXGISwapChain* pSwapChain) {
    if (g_fpPresent == nullptr) {
         void** vtable = *reinterpret_cast<void***>(pSwapChain);
         MH_CreateHook(vtable[8], (LPVOID)hookPresent, (LPVOID*)&g_fpPresent);
         MH_EnableHook(vtable[8]);

         MH_CreateHook(vtable[13], (LPVOID)hookResizeBuffers, (LPVOID*)&g_fpResizeBuffers);
         MH_EnableHook(vtable[13]);
         Log("DXGI SwapChain Hooked!");
    }
}

HRESULT __stdcall hookCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    HRESULT hr = g_fpCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) InstallSwapChainHooks(*ppSwapChain);
    return hr;
}

HRESULT __stdcall hookCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    HRESULT hr = g_fpCreateSwapChainForHwnd(pFactory, pDevice, hWnd, pDesc, pFullDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) InstallSwapChainHooks(*ppSwapChain);
    return hr;
}

void InstallFactoryHooks(IDXGIFactory* pFactory) {
    void** vtable = *reinterpret_cast<void***>(pFactory);
    if (g_fpCreateSwapChain == nullptr) {
        MH_CreateHook(vtable[10], (LPVOID)hookCreateSwapChain, (LPVOID*)&g_fpCreateSwapChain);
        MH_EnableHook(vtable[10]);
    }
    
    IDXGIFactory2* pFactory2 = nullptr;
    if (SUCCEEDED(pFactory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&pFactory2))) {
        void** vtable2 = *reinterpret_cast<void***>(pFactory2);
        if (g_fpCreateSwapChainForHwnd == nullptr) {
             MH_CreateHook(vtable2[15], (LPVOID)hookCreateSwapChainForHwnd, (LPVOID*)&g_fpCreateSwapChainForHwnd);
             MH_EnableHook(vtable2[15]);
        }
        pFactory2->Release();
    }
}

HRESULT __stdcall hookCreateDXGIFactory(REFIID riid, void** ppFactory) {
    HRESULT hr = g_fpCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks((IDXGIFactory*)*ppFactory);
    return hr;
}

HRESULT __stdcall hookCreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HRESULT hr = g_fpCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks((IDXGIFactory*)*ppFactory);
    return hr;
}

HRESULT __stdcall hookCreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    HRESULT hr = g_fpCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks((IDXGIFactory*)*ppFactory);
    return hr;
}

void InitDXGIHook() {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (hDXGI) {
        void* addr;
        addr = (void*)GetProcAddress(hDXGI, "CreateDXGIFactory");
        if(addr) MH_CreateHook(addr, (LPVOID)hookCreateDXGIFactory, (LPVOID*)&g_fpCreateDXGIFactory); 

        addr = (void*)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if(addr) MH_CreateHook(addr, (LPVOID)hookCreateDXGIFactory1, (LPVOID*)&g_fpCreateDXGIFactory1); 

        addr = (void*)GetProcAddress(hDXGI, "CreateDXGIFactory2");
        if(addr) MH_CreateHook(addr, (LPVOID)hookCreateDXGIFactory2, (LPVOID*)&g_fpCreateDXGIFactory2); 

        MH_EnableHook(MH_ALL_HOOKS);
        Log("DXGI Factory Hooks Installed.");
    }

    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (hD3D12) {
        void* addr = (void*)GetProcAddress(hD3D12, "D3D12CreateDevice");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hookD3D12CreateDevice, (LPVOID*)&g_fpD3D12CreateDevice);
            MH_EnableHook(addr);
            Log("D3D12 Export Hooked.");
        }
    }

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (hD3D11) {
        void* addr = (void*)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hookD3D11CreateDeviceAndSwapChain, (LPVOID*)&g_fpD3D11CreateDeviceAndSwapChain);
            MH_EnableHook(addr);
            Log("D3D11 Export Hooked.");
        }
    }
}
