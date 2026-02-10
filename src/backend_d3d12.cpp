#include "global.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MinHook.h" // Needed for ExecuteCommandLists hook

// --- Globals for DX12 Backend ---
static ID3D12Device* g_pd3dDevice12 = nullptr;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr; // We need to capture this
static bool g_init12 = false;

struct FrameContext {
    ID3D12CommandAllocator* CommandAllocator;
    ID3D12Resource* MainRenderTargetResource;
    D3D12_CPU_DESCRIPTOR_HANDLE MainRenderTargetDescriptor;
};
static FrameContext* g_FrameContext = nullptr;
static UINT g_FrameContextCount = 0;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;

// We still need to hook ExecuteCommandLists to capture the queue! 
// This is device-specific, not swapchain specific.
// The device is created by the game. We can't easily hook Device creation from here if we are just called from Present.
// BUT, often the queue is available via `pSwapChain->GetDevice(...)` -> QueryInterface? No.
// We need the queue to SUBMIT commands.
// If we missed the device creation hook, we are in trouble for DX12 unless we can find the queue.
// HACK: Some engines store the queue in a specific way, or we can try to hook `ExecuteCommandLists` on the queue if we can find it.
// The `hook_dxgi.cpp` will handle the logic of "We have a SwapChain".
// If we don't have the CommandQueue, we CANNOT render ImGui in DX12.
// We must hook `CreateCommandQueue` or `ExecuteCommandLists`.
// Since we are moving to `hook_dxgi`, we can't rely on `CreateDevice` hook unless we hook D3D12 exports too.

// We will keep the ExecuteCommandLists hook logic here, but it needs to be installed.
// We can try to install it on the first frame if we can get the queue?
// No, we need the queue *pointer* to hook its vtable.
// We can get the Device from SwapChain.
// From Device, we can't get the Queue easily (it's created separately).
// LUCKILY: We are hooking `D3D12CreateDevice` export in `dllmain.cpp` (or `hook_dxgi.cpp` now!).
// That hook should capture the queue.

extern ID3D12CommandQueue* g_CapturedCommandQueue; // Defined in hook_dxgi.cpp or similar?
// Let's define it here and let the hook in hook_dxgi populate it?
// Actually, let's keep the D3D12 Export hooks in `hook_dxgi.cpp` as well? 
// Yes, `hook_dxgi.cpp` will be the Master Hook File.

// So this file just contains the `RenderDX12` function.

void CreateRenderTarget12(IDXGISwapChain* pSwapChain) {
    for (UINT i = 0; i < g_FrameContextCount; i++) {
        pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_FrameContext[i].MainRenderTargetResource));
        g_pd3dDevice12->CreateRenderTargetView(g_FrameContext[i].MainRenderTargetResource, NULL, g_FrameContext[i].MainRenderTargetDescriptor);
    }
}

void CleanupRenderTarget12() {
    for (UINT i = 0; i < g_FrameContextCount; i++) {
        if (g_FrameContext[i].MainRenderTargetResource) { 
            g_FrameContext[i].MainRenderTargetResource->Release(); 
            g_FrameContext[i].MainRenderTargetResource = NULL; 
        }
    }
    if (g_FrameContext) { delete[] g_FrameContext; g_FrameContext = NULL; }
}

void RenderDX12(IDXGISwapChain* pSwapChain) {
    if (!g_init12) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&g_pd3dDevice12))) {
            DXGI_SWAP_CHAIN_DESC desc;
            pSwapChain->GetDesc(&desc);
            g_hWnd = desc.OutputWindow;
            if (!g_hWnd || !IsWindow(g_hWnd)) g_hWnd = FindMainWindow();

            if (IsWindow(g_hWnd)) {
                WNDPROC current = (WNDPROC)GetWindowLongPtr(g_hWnd, GWLP_WNDPROC);
                if (current != (WNDPROC)WndProc) {
                    g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
                }
            }

            g_FrameContextCount = desc.BufferCount;
            g_FrameContext = new FrameContext[g_FrameContextCount];

            D3D12_DESCRIPTOR_HEAP_DESC rtvdesc = {};
            rtvdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvdesc.NumDescriptors = g_FrameContextCount;
            rtvdesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            rtvdesc.NodeMask = 1;
            if (FAILED(g_pd3dDevice12->CreateDescriptorHeap(&rtvdesc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)))) return;

            SIZE_T rtvDescriptorSize = g_pd3dDevice12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            
            for (UINT i = 0; i < g_FrameContextCount; i++) {
                g_FrameContext[i].MainRenderTargetDescriptor = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
                g_pd3dDevice12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_FrameContext[i].CommandAllocator));
            }

            CreateRenderTarget12(pSwapChain);

            D3D12_DESCRIPTOR_HEAP_DESC srvdesc = {};
            srvdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvdesc.NumDescriptors = 1;
            srvdesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_pd3dDevice12->CreateDescriptorHeap(&srvdesc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)))) return;

            if (FAILED(g_pd3dDevice12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FrameContext[0].CommandAllocator, NULL, IID_PPV_ARGS(&g_pd3dCommandList)))) return;
            g_pd3dCommandList->Close();

            ImGui::CreateContext();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX12_Init(g_pd3dDevice12, g_FrameContextCount,
                DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

            g_RendererName = "Start DirectX 12";
            g_init12 = true;

            // Try to capture or create CommandQueue
            if (!g_CapturedCommandQueue) {
                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                if (FAILED(g_pd3dDevice12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CapturedCommandQueue)))) {
                    Log("Failed to create CommandQueue for DX12");
                    return;
                }
                Log("Created own CommandQueue for DX12");
            }
        }
    }

    if (g_init12 && g_CapturedCommandQueue) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        RenderOverlay();

        ImGui::Render();

        UINT backBufferIdx = ((IDXGISwapChain3*)pSwapChain)->GetCurrentBackBufferIndex();
        FrameContext* frameCtx = &g_FrameContext[backBufferIdx];
        
        frameCtx->CommandAllocator->Reset();
        
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = frameCtx->MainRenderTargetResource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, NULL);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->OMSetRenderTargets(1, &frameCtx->MainRenderTargetDescriptor, FALSE, NULL);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
        
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();
        
        ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
        g_CapturedCommandQueue->ExecuteCommandLists(1, ppCommandLists);
    }
}

void CleanupDX12() {
    if (g_init12) {
        CleanupRenderTarget12();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_init12 = false;
    }
}
