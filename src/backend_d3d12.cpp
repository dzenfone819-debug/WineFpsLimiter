#include "global.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MinHook.h" // Needed for ExecuteCommandLists hook

// --- Globals for DX12 Backend ---
static ID3D12Device* g_pd3dDevice12 = nullptr;
static bool g_init12 = false;

// Synchronization objects
static ID3D12Fence* g_Fence = nullptr;
static HANDLE g_FenceEvent = nullptr;
static UINT64 g_FenceLastSignaledValue = 0;

struct FrameContext {
    ID3D12CommandAllocator* CommandAllocator;
    ID3D12Resource* MainRenderTargetResource;
    D3D12_CPU_DESCRIPTOR_HANDLE MainRenderTargetDescriptor;
    UINT64 FenceValue;
};
static FrameContext* g_FrameContext = nullptr;
static UINT g_FrameContextCount = 0;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;

extern ID3D12CommandQueue* g_CapturedCommandQueue; 
// g_LastCommandQueue removed

// --- Render-to-Texture Composition ---
static ID3D12Resource* g_OverlayTexture = nullptr;
static ID3D12DescriptorHeap* g_OverlayRtvHeap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_OverlayRtvHandle;
static ID3D12RootSignature* g_ComposeRootSig = nullptr;
static ID3D12PipelineState* g_ComposePSO = nullptr;

// Embedded Shaders
const char* g_ComposeShaderCode = 
"struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"PSInput VSMain(uint id : SV_VertexID) {\n"
"    PSInput output;\n"
"    output.uv = float2((id << 1) & 2, id & 2);\n"
"    output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    return output;\n"
"}\n"
"Texture2D g_texture : register(t0);\n"
"SamplerState g_sampler : register(s0);\n"
"float4 PSMain(PSInput input) : SV_TARGET {\n"
"    return g_texture.Sample(g_sampler, input.uv);\n"
"}\n";

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

bool CreateOverlayResources(IDXGISwapChain* pSwapChain) {
    DXGI_SWAP_CHAIN_DESC desc;
    pSwapChain->GetDesc(&desc);
    
    // Create Overlay Texture
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = desc.BufferDesc.Width;
    texDesc.Height = desc.BufferDesc.Height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(clearValue.Color, clearColor, sizeof(float) * 4);

    if (FAILED(g_pd3dDevice12->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, 
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, 
        IID_PPV_ARGS(&g_OverlayTexture)))) return false;

    // Create RTV Heap for Overlay
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_pd3dDevice12->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_OverlayRtvHeap)))) return false;

    g_OverlayRtvHandle = g_OverlayRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_pd3dDevice12->CreateRenderTargetView(g_OverlayTexture, NULL, g_OverlayRtvHandle);

    // Create Root Signature
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    
    // Add Static Sampler
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = &param;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* signature = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) return false;
    if (FAILED(g_pd3dDevice12->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_ComposeRootSig)))) return false;
    signature->Release();

    // Compile Shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (FAILED(D3DCompile(g_ComposeShaderCode, strlen(g_ComposeShaderCode), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vsBlob, NULL))) return false;
    if (FAILED(D3DCompile(g_ComposeShaderCode, strlen(g_ComposeShaderCode), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &psBlob, NULL))) return false;

    // Create PSO manually (no D3DX12 helpers)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_ComposeRootSig;
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    
    // Rasterizer State
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // Full screen quad
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    
    // Blend State
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
    {
        FALSE,FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL
    };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        psoDesc.BlendState.RenderTarget[i] = defaultRenderTargetBlendDesc;
    
    // Enable Alpha Blending for RT[0]
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; 
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = desc.BufferDesc.Format; 
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(g_pd3dDevice12->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_ComposePSO)))) return false;

    vsBlob->Release();
    psBlob->Release();

    return true;
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
                g_FrameContext[i].FenceValue = 0;
                rtvHandle.ptr += rtvDescriptorSize;
                g_pd3dDevice12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_FrameContext[i].CommandAllocator));
            }

            CreateRenderTarget12(pSwapChain);

            // SRV Heap: Increase size to 2 (1 for Font, 1 for Composition Texture)
            D3D12_DESCRIPTOR_HEAP_DESC srvdesc = {};
            srvdesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvdesc.NumDescriptors = 2; // Increased size
            srvdesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_pd3dDevice12->CreateDescriptorHeap(&srvdesc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)))) return;

            if (FAILED(g_pd3dDevice12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FrameContext[0].CommandAllocator, NULL, IID_PPV_ARGS(&g_pd3dCommandList)))) return;
            g_pd3dCommandList->Close();

            // --- FENCE CREATION ---
            if (FAILED(g_pd3dDevice12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence)))) return;
            g_FenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (g_FenceEvent == NULL) return;

            // --- OVERLAY RESOURCES ---
            if (!CreateOverlayResources(pSwapChain)) {
                Log("Failed to create Overlay Resources!");
                return;
            }

            // Create SRV for Overlay Texture in Slot 1
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            
            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
            SIZE_T srvSize = g_pd3dDevice12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            
            D3D12_CPU_DESCRIPTOR_HANDLE overlaySrvHandle = srvHandle;
            overlaySrvHandle.ptr += srvSize;
            g_pd3dDevice12->CreateShaderResourceView(g_OverlayTexture, &srvDesc, overlaySrvHandle);


            ImGui::CreateContext();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX12_Init(g_pd3dDevice12, g_FrameContextCount,
                DXGI_FORMAT_R8G8B8A8_UNORM, g_pd3dSrvDescHeap,
                g_pd3dSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

            g_RendererName = "Start DirectX 12";

            ImGuiIO& io = ImGui::GetIO();
            unsigned char* pixels = nullptr; int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); 
            if (!ImGui_ImplDX12_CreateDeviceObjects()) {
                Log("ImGui: ImGui_ImplDX12_CreateDeviceObjects() failed!");
            }

            g_init12 = true;

            // Fallback if no queue was captured (rare with new SwapChain hook)
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

    // Always use g_CapturedCommandQueue (Static Capture)
    ID3D12CommandQueue* pCommandQueue = g_CapturedCommandQueue;

    if (g_init12 && pCommandQueue) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        RenderOverlay();

        ImGui::Render();

        UINT backBufferIdx = 0;
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
            backBufferIdx = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        } else {
            Log("Warning: SwapChain does not support IDXGISwapChain3; using backBufferIdx=0");
        }
        FrameContext* frameCtx = &g_FrameContext[backBufferIdx];
        
        // Wait for fence before reusing allocator (Standard Sync)
        if (frameCtx->FenceValue != 0) {
            if (g_Fence->GetCompletedValue() < frameCtx->FenceValue) {
                g_Fence->SetEventOnCompletion(frameCtx->FenceValue, g_FenceEvent);
                WaitForSingleObject(g_FenceEvent, INFINITE);
            }
        }

        frameCtx->CommandAllocator->Reset();
        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, NULL);
        
        // --- 1. RENDER IMGUI TO OVERLAY TEXTURE ---
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = g_OverlayTexture;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // Assumed state from last frame
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_pd3dCommandList->ResourceBarrier(1, &barrier);

            float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            g_pd3dCommandList->ClearRenderTargetView(g_OverlayRtvHandle, clearColor, 0, NULL);
            g_pd3dCommandList->OMSetRenderTargets(1, &g_OverlayRtvHandle, FALSE, NULL);
            g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
            
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_pd3dCommandList->ResourceBarrier(1, &barrier);
        }

        // --- 2. COMPOSITE TO BACK BUFFER ---
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = frameCtx->MainRenderTargetResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_pd3dCommandList->ResourceBarrier(1, &barrier);

            g_pd3dCommandList->OMSetRenderTargets(1, &frameCtx->MainRenderTargetDescriptor, FALSE, NULL);
            
            // Set Viewport/Scissor to match backbuffer
            D3D12_RESOURCE_DESC bbDesc = frameCtx->MainRenderTargetResource->GetDesc();
            D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)bbDesc.Width, (float)bbDesc.Height, 0.0f, 1.0f };
            D3D12_RECT scissor = { 0, 0, (LONG)bbDesc.Width, (LONG)bbDesc.Height };
            g_pd3dCommandList->RSSetViewports(1, &vp);
            g_pd3dCommandList->RSSetScissorRects(1, &scissor);

            g_pd3dCommandList->SetPipelineState(g_ComposePSO);
            g_pd3dCommandList->SetGraphicsRootSignature(g_ComposeRootSig);
            
            // Bind SRV Heap and Slot 1 (Overlay Texture)
            g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
            
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = g_pd3dSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
            SIZE_T srvSize = g_pd3dDevice12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            srvGpuHandle.ptr += srvSize; // Point to slot 1
            
            g_pd3dCommandList->SetGraphicsRootDescriptorTable(0, srvGpuHandle);
            g_pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_pd3dCommandList->DrawInstanced(3, 1, 0, 0);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_pd3dCommandList->ResourceBarrier(1, &barrier);
        }

        g_pd3dCommandList->Close();
        
        ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
        pCommandQueue->ExecuteCommandLists(1, ppCommandLists);

        // --- SIGNAL FENCE ---
        g_FenceLastSignaledValue++;
        pCommandQueue->Signal(g_Fence, g_FenceLastSignaledValue);
        frameCtx->FenceValue = g_FenceLastSignaledValue;

        // --- FLUSH QUEUE (Timeout: 2000ms) ---
        // Prevents deadlocks if the GPU hangs or we are on the wrong queue.
        if (g_Fence->GetCompletedValue() < g_FenceLastSignaledValue) {
            g_Fence->SetEventOnCompletion(g_FenceLastSignaledValue, g_FenceEvent);
            WaitForSingleObject(g_FenceEvent, 2000); // 2 second timeout
        }
    }
}

void CleanupDX12() {
    if (g_init12) {
        CleanupRenderTarget12();
        
        if (g_Fence) { g_Fence->Release(); g_Fence = nullptr; }
        if (g_FenceEvent) { CloseHandle(g_FenceEvent); g_FenceEvent = nullptr; }
        
        if (g_OverlayTexture) { g_OverlayTexture->Release(); g_OverlayTexture = nullptr; }
        if (g_OverlayRtvHeap) { g_OverlayRtvHeap->Release(); g_OverlayRtvHeap = nullptr; }
        if (g_ComposeRootSig) { g_ComposeRootSig->Release(); g_ComposeRootSig = nullptr; }
        if (g_ComposePSO) { g_ComposePSO->Release(); g_ComposePSO = nullptr; }

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_init12 = false;
    }
}
