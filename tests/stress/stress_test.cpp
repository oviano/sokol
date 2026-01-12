// ---------------------------------------------------------------------------------------------------------------------
//
// stress_test.cpp - D3D12 backend validation stress test
//
// purpose: validate sokol_gfx D3D12 backend performance against D3D11 baseline
//
// this stress test exercises:
// - individual draws (legacy approach with per-draw uniform updates)
// - batched uniforms (storage buffer technique with indexed access)
// - gpu instancing (standard modern technique for repeated geometry)
// - dynamic buffer and texture updates
// - multi-pass rendering
// - compute dispatches (D3D12 only)
//
// test scenarios simulate realistic game workloads from AAA to extreme stress cases
//
// build with SOKOL_D3D11 or SOKOL_D3D12 defined to select backend
//
// usage: stress-test-<backend>.exe [options]
//   see README.md for complete command-line reference and tiered test scenarios
//
// ---------------------------------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <atlbase.h>

#if defined(SOKOL_D3D12)
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#pragma comment(lib, "d3d12.lib")
#elif defined(SOKOL_D3D11)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#pragma comment(lib, "d3d11.lib")
#else
#error "Define SOKOL_D3D11 or SOKOL_D3D12"
#endif

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

// ---------------------------------------------------------------------------------------------------------------------

#define SOKOL_IMPL
#include "../sokol_gfx.h"

// ---------------------------------------------------------------------------------------------------------------------

constexpr UINT FRAME_COUNT = 2;
constexpr UINT WINDOW_WIDTH = 1920;
constexpr UINT WINDOW_HEIGHT = 1080;

// ---------------------------------------------------------------------------------------------------------------------

#if defined(SOKOL_D3D12)

struct GFX_STATE
{
    // device and command queue
    CComPtr<ID3D12Device> device;
    CComPtr<ID3D12CommandQueue> command_queue;

    // swap chain
    CComPtr<IDXGISwapChain3> swap_chain;
    UINT frame_index = 0;
    bool tearing_supported = false;

    // render targets
    CComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_descriptor_size = 0;
    CComPtr<ID3D12Resource> render_targets[FRAME_COUNT];

    // depth stencil
    CComPtr<ID3D12DescriptorHeap> dsv_heap;
    CComPtr<ID3D12Resource> depth_stencil;

    // synchronization
    CComPtr<ID3D12Fence> fence;
    UINT64 fence_values[FRAME_COUNT] = {};
    UINT64 next_fence_value = 1;
    HANDLE fence_event = nullptr;

    // window
    HWND hwnd = nullptr;
    UINT width = WINDOW_WIDTH;
    UINT height = WINDOW_HEIGHT;
    bool resized = false;
    bool running = true;
};

#else // SOKOL_D3D11

struct GFX_STATE
{
    // device and context
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;

    // swap chain
    CComPtr<IDXGISwapChain1> swap_chain;
    bool tearing_supported = false;

    // render targets
    CComPtr<ID3D11RenderTargetView> rtv;
    CComPtr<ID3D11Texture2D> depth_stencil;
    CComPtr<ID3D11DepthStencilView> dsv;

    // window
    HWND hwnd = nullptr;
    UINT width = WINDOW_WIDTH;
    UINT height = WINDOW_HEIGHT;
    bool resized = false;
    bool running = true;
};

#endif

static GFX_STATE g_state;

// ---------------------------------------------------------------------------------------------------------------------
// TEST CONFIGURATION
// ---------------------------------------------------------------------------------------------------------------------

struct TEST_CONFIG
{
    // operations per frame
    int num_draws = 0;              // number of draw calls
    int num_instanced_draws = 0;    // number of instances in ONE instanced draw call
    int num_batched_draws = 0;      // number of draws using storage buffer batching
    int num_state_changes = 0;      // number of pipeline/binding changes
    int num_buffer_updates = 0;     // number of dynamic buffer updates
    int num_texture_updates = 0;    // number of texture updates
    int num_passes = 1;             // number of render passes (default 1)
    int num_compute_dispatches = 0; // number of compute dispatches (D3D12 only)

    // resource pool sizes (for state change tests)
    int num_pipelines = 10;
    int num_textures = 25;
};

struct PERF_METRICS
{
    float frame_time_ms = 0.0f;
    float fps = 0.0f;
    int draw_calls = 0;
    int state_changes = 0;
    int buffer_updates = 0;
    int texture_updates = 0;
    int compute_dispatches = 0;

    // rolling average (last 60 frames)
    std::vector<float> frame_times;
    int frame_index = 0;

    // detailed timing breakdown (microseconds)
    double total_commit_us = 0.0;
    double total_present_us = 0.0;
    double total_wait_us = 0.0;
    int timing_samples = 0;
};

static TEST_CONFIG g_config;
static PERF_METRICS g_metrics;

// log file for unattended testing
static FILE* g_log_file = nullptr;

// ---------------------------------------------------------------------------------------------------------------------

static void log_output(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    // always print to console
    vprintf(format, args);

    // also write to log file if enabled
    if (g_log_file)
    {
        va_start(args, format);
        vfprintf(g_log_file, format, args);
        fflush(g_log_file);  // ensure it's written immediately
    }

    va_end(args);
}

static void update_stress_config()
{
    // print current test configuration
    log_output("\n");
    log_output("========================================\n");
    log_output("Test Configuration:\n");
    log_output("  Draws: %d\n", g_config.num_draws);
    log_output("  Instanced Draws: %d\n", g_config.num_instanced_draws);
    log_output("  State Changes: %d\n", g_config.num_state_changes);
    log_output("  Buffer Updates: %d\n", g_config.num_buffer_updates);
    log_output("  Texture Updates: %d\n", g_config.num_texture_updates);
    log_output("  Passes: %d\n", g_config.num_passes);
    log_output("  Compute Dispatches: %d\n", g_config.num_compute_dispatches);
    log_output("  Pipelines: %d\n", g_config.num_pipelines);
    log_output("  Textures: %d\n", g_config.num_textures);
    log_output("========================================\n");
    log_output("\n");
}

// ---------------------------------------------------------------------------------------------------------------------

#if defined(SOKOL_D3D12)

static void wait_for_gpu()
{
    // signal fence with new value to ensure all in-flight frames complete
    const UINT64 fence_value = g_state.next_fence_value++;
    g_state.command_queue->Signal(g_state.fence, fence_value);

    // wait for all GPU work to complete
    if (g_state.fence->GetCompletedValue() < fence_value)
    {
        g_state.fence->SetEventOnCompletion(fence_value, g_state.fence_event);
        WaitForSingleObject(g_state.fence_event, INFINITE);
    }

    // reset all per-frame fence values since all work is now complete
    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        g_state.fence_values[i] = 0;
    }
}

static void wait_for_previous_frame()
{
    // signal fence for current frame's GPU work
    const UINT64 fence_value_to_signal = g_state.next_fence_value++;
    g_state.command_queue->Signal(g_state.fence, fence_value_to_signal);
    g_state.fence_values[g_state.frame_index] = fence_value_to_signal;

    // move to next frame
    g_state.frame_index = g_state.swap_chain->GetCurrentBackBufferIndex();

    // wait if next frame's GPU work hasn't completed
    const UINT64 fence_value_to_wait_for = g_state.fence_values[g_state.frame_index];
    if (fence_value_to_wait_for > 0 && g_state.fence->GetCompletedValue() < fence_value_to_wait_for)
    {
        g_state.fence->SetEventOnCompletion(fence_value_to_wait_for, g_state.fence_event);
        WaitForSingleObject(g_state.fence_event, INFINITE);
    }
}

#endif

// ---------------------------------------------------------------------------------------------------------------------

static bool create_device()
{
#if defined(SOKOL_D3D12)
    // enable debug layer in debug builds
#if defined(_DEBUG)
    CComPtr<ID3D12Debug> debug_controller;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))
    {
        debug_controller->EnableDebugLayer();
        log_output("D3D12 debug layer enabled\n");
    }
#endif

    // create dxgi factory
    CComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        log_output("Error: failed to create DXGI factory\n");
        return false;
    }

    // check for tearing support
    CComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            g_state.tearing_supported = (allowTearing == TRUE);
        }
    }

    if (g_state.tearing_supported)
    {
        log_output("Tearing support: YES (VSync can be disabled)\n");
    }
    else
    {
        log_output("Tearing support: NO (VSync will be forced on)\n");
    }

    // find hardware adapter
    CComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // skip software adapter
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Release();
            continue;
        }

        // try to create device
        if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_state.device))))
        {
            log_output("Using adapter: %ls\n", desc.Description);
            break;
        }

        adapter.Release();
    }

    if (!g_state.device)
    {
        log_output("Error: failed to create D3D12 device\n");
        return false;
    }

    // create command queue
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (FAILED(g_state.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g_state.command_queue))))
    {
        log_output("Error: failed to create command queue\n");
        return false;
    }

    // create swap chain
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = FRAME_COUNT;
    swap_chain_desc.Width = g_state.width;
    swap_chain_desc.Height = g_state.height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.SampleDesc.Count = 1;
    if (g_state.tearing_supported)
    {
        swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    CComPtr<IDXGISwapChain1> swap_chain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_state.command_queue, g_state.hwnd, &swap_chain_desc, nullptr, nullptr,
                                               &swap_chain1)))
    {
        log_output("Error: failed to create swap chain\n");
        return false;
    }

    // disable alt-enter
    factory->MakeWindowAssociation(g_state.hwnd, DXGI_MWA_NO_ALT_ENTER);

    // get swap chain 3
    if (FAILED(swap_chain1->QueryInterface(IID_PPV_ARGS(&g_state.swap_chain))))
    {
        log_output("Error: failed to get swap chain 3\n");
        return false;
    }

    g_state.frame_index = g_state.swap_chain->GetCurrentBackBufferIndex();

    // create rtv descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(g_state.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&g_state.rtv_heap))))
    {
        log_output("Error: failed to create RTV descriptor heap\n");
        return false;
    }

    g_state.rtv_descriptor_size = g_state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // create dsv descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(g_state.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&g_state.dsv_heap))))
    {
        log_output("Error: failed to create DSV descriptor heap\n");
        return false;
    }

    // create fence
    if (FAILED(g_state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_state.fence))))
    {
        log_output("Error: failed to create fence\n");
        return false;
    }

    g_state.fence_values[g_state.frame_index]++;

    // create fence event
    g_state.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_state.fence_event)
    {
        log_output("Error: failed to create fence event\n");
        return false;
    }

    log_output("D3D12 device created successfully\n");

#else // SOKOL_D3D11

    // create dxgi factory
    CComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        log_output("Error: failed to create DXGI factory\n");
        return false;
    }

    // check for tearing support
    CComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            g_state.tearing_supported = (allowTearing == TRUE);
        }
    }

    if (g_state.tearing_supported)
    {
        log_output("Tearing support: YES (VSync can be disabled)\n");
    }
    else
    {
        log_output("Tearing support: NO (VSync will be forced on)\n");
    }

    // find hardware adapter
    CComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // skip software adapter
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Release();
            continue;
        }

        log_output("Using adapter: %ls\n", desc.Description);
        break;
    }

    // create device and context
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL feature_level;

    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, feature_levels, 1, D3D11_SDK_VERSION,
                                 &g_state.device, &feature_level, &g_state.context)))
    {
        log_output("Error: failed to create D3D11 device\n");
        return false;
    }

#if defined(_DEBUG)
    log_output("D3D11 debug layer enabled\n");
#endif

    // create swap chain
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = FRAME_COUNT;
    swap_chain_desc.Width = g_state.width;
    swap_chain_desc.Height = g_state.height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.SampleDesc.Count = 1;
    if (g_state.tearing_supported)
    {
        swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    if (FAILED(factory->CreateSwapChainForHwnd(g_state.device, g_state.hwnd, &swap_chain_desc, nullptr, nullptr,
                                               &g_state.swap_chain)))
    {
        log_output("Error: failed to create swap chain\n");
        return false;
    }

    // disable alt-enter
    factory->MakeWindowAssociation(g_state.hwnd, DXGI_MWA_NO_ALT_ENTER);

    log_output("D3D11 device created successfully\n");

#endif

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

static bool create_render_targets()
{
#if defined(SOKOL_D3D12)
    // create render target views
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_state.rtv_heap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        if (FAILED(g_state.swap_chain->GetBuffer(i, IID_PPV_ARGS(&g_state.render_targets[i]))))
        {
            log_output("Error: failed to get swap chain buffer %u\n", i);
            return false;
        }

        g_state.device->CreateRenderTargetView(g_state.render_targets[i], nullptr, rtv_handle);
        rtv_handle.ptr += g_state.rtv_descriptor_size;
    }

    // create depth stencil
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC ds_desc = {};
    ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ds_desc.Width = g_state.width;
    ds_desc.Height = g_state.height;
    ds_desc.DepthOrArraySize = 1;
    ds_desc.MipLevels = 1;
    ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds_desc.SampleDesc.Count = 1;
    ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    if (FAILED(g_state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ds_desc,
                                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                                       IID_PPV_ARGS(&g_state.depth_stencil))))
    {
        log_output("Error: failed to create depth stencil\n");
        return false;
    }

    // create depth stencil view
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = g_state.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    g_state.device->CreateDepthStencilView(g_state.depth_stencil, nullptr, dsv_handle);

#else // SOKOL_D3D11

    // get back buffer
    CComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(g_state.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
    {
        log_output("Error: failed to get swap chain buffer\n");
        return false;
    }

    // create render target view
    if (FAILED(g_state.device->CreateRenderTargetView(back_buffer, nullptr, &g_state.rtv)))
    {
        log_output("Error: failed to create render target view\n");
        return false;
    }

    // create depth stencil texture
    D3D11_TEXTURE2D_DESC ds_desc = {};
    ds_desc.Width = g_state.width;
    ds_desc.Height = g_state.height;
    ds_desc.MipLevels = 1;
    ds_desc.ArraySize = 1;
    ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds_desc.SampleDesc.Count = 1;
    ds_desc.Usage = D3D11_USAGE_DEFAULT;
    ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    if (FAILED(g_state.device->CreateTexture2D(&ds_desc, nullptr, &g_state.depth_stencil)))
    {
        log_output("Error: failed to create depth stencil texture\n");
        return false;
    }

    // create depth stencil view
    if (FAILED(g_state.device->CreateDepthStencilView(g_state.depth_stencil, nullptr, &g_state.dsv)))
    {
        log_output("Error: failed to create depth stencil view\n");
        return false;
    }

#endif

    log_output("Render targets created: %ux%u\n", g_state.width, g_state.height);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

static void destroy_render_targets()
{
#if defined(SOKOL_D3D12)
    g_state.depth_stencil.Release();

    for (UINT i = 0; i < FRAME_COUNT; i++)
    {
        g_state.render_targets[i].Release();
    }
#else // SOKOL_D3D11
    g_state.context->OMSetRenderTargets(0, nullptr, nullptr);
    g_state.dsv.Release();
    g_state.depth_stencil.Release();
    g_state.rtv.Release();
#endif
}

// ---------------------------------------------------------------------------------------------------------------------

static bool resize_render_targets()
{
#if defined(SOKOL_D3D12)
    // wait for gpu to finish
    wait_for_gpu();
#endif

    // destroy old render targets
    destroy_render_targets();

    // resize swap chain with appropriate flags
    UINT swap_chain_flags = g_state.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(g_state.swap_chain->ResizeBuffers(FRAME_COUNT, g_state.width, g_state.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 swap_chain_flags)))
    {
        log_output("Error: failed to resize swap chain\n");
        return false;
    }

#if defined(SOKOL_D3D12)
    g_state.frame_index = g_state.swap_chain->GetCurrentBackBufferIndex();
#endif

    // recreate render targets
    return create_render_targets();
}

// ---------------------------------------------------------------------------------------------------------------------

static void destroy_device()
{
#if defined(SOKOL_D3D12)
    wait_for_gpu();

    if (g_state.fence_event)
    {
        CloseHandle(g_state.fence_event);
        g_state.fence_event = nullptr;
    }
#endif

    destroy_render_targets();
}

// ---------------------------------------------------------------------------------------------------------------------

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            g_state.width = LOWORD(lparam);
            g_state.height = HIWORD(lparam);
            if (g_state.width > 0 && g_state.height > 0)
            {
                g_state.resized = true;
            }
        }
        return 0;

    case WM_DESTROY:
        g_state.running = false;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            g_state.running = false;
            PostQuitMessage(0);
        }
        // keyboard controls removed - use command line parameters instead
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------------------------------------------------

static bool create_window()
{
    // register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"StressTest";

    if (!RegisterClassExW(&wc))
    {
        log_output("Error: failed to register window class\n");
        return false;
    }

    // calculate window size
    RECT rect = {0, 0, (LONG)g_state.width, (LONG)g_state.height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // create window
#if defined(SOKOL_D3D12)
    const wchar_t* title = L"D3D12 Stress Test";
#else
    const wchar_t* title = L"D3D11 Stress Test";
#endif

    g_state.hwnd =
        CreateWindowExW(0, L"StressTest", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!g_state.hwnd)
    {
        log_output("Error: failed to create window\n");
        return false;
    }

    ShowWindow(g_state.hwnd, SW_SHOW);

    log_output("Window created: %ux%u\n", g_state.width, g_state.height);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// SHADERS
// ---------------------------------------------------------------------------------------------------------------------

// basic shader with uniform color tint
static const char* basic_vs_source =
    "cbuffer vs_params : register(b0) {\n"
    "  float4x4 mvp;\n"
    "};\n"
    "struct vs_in {\n"
    "  float4 pos: POSITION;\n"
    "  float2 uv: TEXCOORD0;\n"
    "};\n"
    "struct vs_out {\n"
    "  float2 uv: TEXCOORD0;\n"
    "  float4 pos: SV_Position;\n"
    "};\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  outp.pos = mul(mvp, inp.pos);\n"
    "  outp.uv = inp.uv;\n"
    "  return outp;\n"
    "}\n";

static const char* basic_fs_source =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer fs_params : register(b0) {\n"
    "  float4 tint;\n"
    "};\n"
    "float4 main(float2 uv: TEXCOORD0): SV_Target0 {\n"
    "  return tex.Sample(smp, uv) * tint;\n"
    "}\n";

// compute shader (D3D12 only)
static const char* compute_cs_source =
    "RWStructuredBuffer<float4> output : register(u0);\n"
    "cbuffer cs_params : register(b0) {\n"
    "  float time;\n"
    "  float3 _pad;\n"
    "};\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 dtid : SV_DispatchThreadID) {\n"
    "  uint idx = dtid.x;\n"
    "  float t = time + idx * 0.1;\n"
    "  output[idx] = float4(\n"
    "    0.5 + 0.5 * sin(t),\n"
    "    0.5 + 0.5 * sin(t + 2.094),\n"
    "    0.5 + 0.5 * sin(t + 4.189),\n"
    "    1.0);\n"
    "}\n";

// instanced vertex shader - reads per-instance data from vertex buffer slot 1
static const char* instanced_vs_source =
    "struct vs_in {\n"
    "  float2 pos : POSITION;\n"
    "  float4 inst_col0 : TEXCOORD0;\n"
    "  float4 inst_col1 : TEXCOORD1;\n"
    "  float4 inst_col2 : TEXCOORD2;\n"
    "  float4 inst_col3 : TEXCOORD3;\n"
    "  float4 inst_tint : TEXCOORD4;\n"
    "};\n"
    "struct vs_out {\n"
    "  float4 color : COLOR0;\n"
    "  float4 pos : SV_Position;\n"
    "};\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  float4x4 mvp = float4x4(inp.inst_col0, inp.inst_col1, inp.inst_col2, inp.inst_col3);\n"
    "  outp.pos = mul(mvp, float4(inp.pos, 0.0, 1.0));\n"
    "  outp.color = inp.inst_tint;\n"
    "  return outp;\n"
    "}\n";

// instanced fragment shader - no uniforms needed, color comes from vertex shader
static const char* instanced_fs_source =
    "float4 main(float4 color : COLOR0) : SV_Target0 {\n"
    "  return color;\n"
    "}\n";

// batched vertex shader - reads from storage buffer indexed by uniform
static const char* batched_vs_source =
    "struct TransformData {\n"
    "  float4x4 mvp;\n"
    "  float4 tint;\n"
    "};\n"
    "StructuredBuffer<TransformData> transforms : register(t0);\n"
    "cbuffer draw_params : register(b0) {\n"
    "  uint draw_id;\n"
    "};\n"
    "struct vs_in {\n"
    "  float4 pos: POSITION;\n"
    "  float2 uv: TEXCOORD0;\n"
    "};\n"
    "struct vs_out {\n"
    "  float4 color: COLOR0;\n"
    "  float2 uv: TEXCOORD0;\n"
    "  float4 pos: SV_Position;\n"
    "};\n"
    "vs_out main(vs_in inp) {\n"
    "  vs_out outp;\n"
    "  TransformData t = transforms[draw_id];\n"
    "  outp.pos = mul(t.mvp, inp.pos);\n"
    "  outp.color = t.tint;\n"
    "  outp.uv = inp.uv;\n"
    "  return outp;\n"
    "}\n";

// batched fragment shader - color from vertex shader, texture optional
static const char* batched_fs_source =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 main(float4 color: COLOR0, float2 uv: TEXCOORD0): SV_Target0 {\n"
    "  return tex.Sample(smp, uv) * color;\n"
    "}\n";

// ---------------------------------------------------------------------------------------------------------------------
// UNIFORM STRUCTURES
// ---------------------------------------------------------------------------------------------------------------------

struct vs_params_t
{
    float mvp[16];
};

struct fs_params_t
{
    float tint[4];
};

struct cs_params_t
{
    float time;
    float _pad[3];
};

struct InstanceData
{
    float mvp[16];   // 4x4 matrix
    float tint[4];   // RGBA color
};  // Total: 80 bytes per instance

struct TransformData
{
    float mvp[16];   // 4x4 matrix
    float tint[4];   // RGBA color
};  // Total: 80 bytes, same as InstanceData but used for storage buffer

struct DrawIdParams
{
    uint32_t draw_id;
    uint32_t _pad[3];  // Pad to 16 bytes for uniform alignment
};

// ---------------------------------------------------------------------------------------------------------------------
// MATRIX HELPERS
// ---------------------------------------------------------------------------------------------------------------------

static void mat4_identity(float* m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_ortho(float* m, float left, float right, float bottom, float top, float near_val, float far_val)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (right - left);
    m[5] = 2.0f / (top - bottom);
    m[10] = -2.0f / (far_val - near_val);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far_val + near_val) / (far_val - near_val);
    m[15] = 1.0f;
}

static void mat4_translate(float* m, float x, float y, float z)
{
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void mat4_scale(float* m, float x, float y, float z)
{
    mat4_identity(m);
    m[0] = x;
    m[5] = y;
    m[10] = z;
}

static void mat4_rotate_z(float* m, float angle)
{
    mat4_identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    m[0] = c;
    m[1] = s;
    m[4] = -s;
    m[5] = c;
}

static void mat4_multiply(float* out, const float* a, const float* b)
{
    float tmp[16];
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            tmp[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                tmp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
            }
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

// ---------------------------------------------------------------------------------------------------------------------
// STRESS TEST RESOURCES
// ---------------------------------------------------------------------------------------------------------------------

struct STRESS_RESOURCES
{
    // quad geometry (shared by all objects)
    sg_buffer quad_vbuf;
    sg_buffer quad_ibuf;

    // textures
    std::vector<sg_image> textures;
    std::vector<sg_view> texture_views;
    std::vector<sg_sampler> samplers;

    // pipelines
    std::vector<sg_shader> shaders;
    std::vector<sg_pipeline> pipelines;

    // dynamic buffers for updates
    std::vector<sg_buffer> dynamic_buffers;

    // offscreen render targets
    std::vector<sg_image> render_targets;
    std::vector<sg_view> rt_color_views;
    std::vector<sg_view> rt_texture_views;

    // compute resources
    std::vector<sg_buffer> compute_buffers;
    std::vector<sg_view> compute_views;
    sg_shader compute_shader;
    sg_pipeline compute_pipeline;

    // instancing resources
    sg_shader instanced_shader;
    sg_pipeline instanced_pipeline;
    sg_buffer instance_buffer;
    std::vector<InstanceData> instance_data;

    // batched resources
    sg_shader batched_shader;
    sg_pipeline batched_pipeline;
    sg_buffer transform_buffer;
    sg_view transform_view;
    std::vector<TransformData> transform_data;

    // object instances
    struct OBJECT
    {
        float x, y;
        float scale;
        float rotation;
        float rotation_speed;
        int texture_index;
        int pipeline_index;
        float tint[4];
    };
    std::vector<OBJECT> objects;
};

static STRESS_RESOURCES g_resources;

// ---------------------------------------------------------------------------------------------------------------------

static bool create_stress_resources()
{
    printf("Creating stress test resources...\n");

    // create quad geometry
    float quad_vertices[] = {
        // pos (x,y,z,w)              // uv (u,v)
        -0.5f,  0.5f, 0.5f, 1.0f,     0.0f, 0.0f,  // top-left
         0.5f,  0.5f, 0.5f, 1.0f,     1.0f, 0.0f,  // top-right
         0.5f, -0.5f, 0.5f, 1.0f,     1.0f, 1.0f,  // bottom-right
        -0.5f, -0.5f, 0.5f, 1.0f,     0.0f, 1.0f,  // bottom-left
    };
    sg_buffer_desc quad_vbuf_desc = {};
    quad_vbuf_desc.data = SG_RANGE(quad_vertices);
    quad_vbuf_desc.label = "quad-vertices";
    g_resources.quad_vbuf = sg_make_buffer(&quad_vbuf_desc);

    uint16_t quad_indices[] = { 0, 1, 2, 0, 2, 3 };
    sg_buffer_desc quad_ibuf_desc = {};
    quad_ibuf_desc.usage.index_buffer = true;
    quad_ibuf_desc.data = SG_RANGE(quad_indices);
    quad_ibuf_desc.label = "quad-indices";
    g_resources.quad_ibuf = sg_make_buffer(&quad_ibuf_desc);

    // create textures (procedural patterns)
    g_resources.textures.resize(g_config.num_textures);
    g_resources.texture_views.resize(g_config.num_textures);

    constexpr int TEX_SIZE = 64;
    uint32_t pixels[TEX_SIZE * TEX_SIZE];

    for (int t = 0; t < g_config.num_textures; t++)
    {
        // generate procedural pattern
        for (int y = 0; y < TEX_SIZE; y++)
        {
            for (int x = 0; x < TEX_SIZE; x++)
            {
                float fx = (float)x / TEX_SIZE;
                float fy = (float)y / TEX_SIZE;
                float v = 0.0f;

                // different pattern for each texture
                switch (t % 4)
                {
                case 0: // checkerboard
                    v = ((x ^ y) & 8) ? 1.0f : 0.0f;
                    break;
                case 1: // stripes
                    v = (x & 8) ? 1.0f : 0.0f;
                    break;
                case 2: // gradient
                    v = fx;
                    break;
                case 3: // radial
                    v = sqrtf((fx - 0.5f) * (fx - 0.5f) + (fy - 0.5f) * (fy - 0.5f)) * 2.0f;
                    break;
                }

                // color variation based on texture index
                float r = v * (0.5f + 0.5f * sinf(t * 0.5f));
                float g = v * (0.5f + 0.5f * sinf(t * 0.5f + 2.094f));
                float b = v * (0.5f + 0.5f * sinf(t * 0.5f + 4.189f));

                uint8_t ir = (uint8_t)(r * 255);
                uint8_t ig = (uint8_t)(g * 255);
                uint8_t ib = (uint8_t)(b * 255);

                pixels[y * TEX_SIZE + x] = (255 << 24) | (ib << 16) | (ig << 8) | ir;
            }
        }

        sg_image_desc img_desc = {};
        img_desc.width = TEX_SIZE;
        img_desc.height = TEX_SIZE;
        img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        img_desc.data.mip_levels[0] = SG_RANGE(pixels);
        char label[64];
        sprintf_s(label, "texture-%d", t);
        img_desc.label = label;
        g_resources.textures[t] = sg_make_image(&img_desc);

        sg_view_desc view_desc = {};
        view_desc.texture.image = g_resources.textures[t];
        sprintf_s(label, "texture-view-%d", t);
        view_desc.label = label;
        g_resources.texture_views[t] = sg_make_view(&view_desc);
    }

    // create samplers
    g_resources.samplers.resize(4);
    for (int s = 0; s < 4; s++)
    {
        sg_sampler_desc smp_desc = {};
        smp_desc.min_filter = (s & 1) ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
        smp_desc.mag_filter = (s & 2) ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
        smp_desc.wrap_u = SG_WRAP_REPEAT;
        smp_desc.wrap_v = SG_WRAP_REPEAT;
        char label[64];
        sprintf_s(label, "sampler-%d", s);
        smp_desc.label = label;
        g_resources.samplers[s] = sg_make_sampler(&smp_desc);
    }

    // create shaders and pipelines
    g_resources.shaders.resize(g_config.num_pipelines);
    g_resources.pipelines.resize(g_config.num_pipelines);

    for (int p = 0; p < g_config.num_pipelines; p++)
    {
        sg_shader_desc shd_desc = {};
        shd_desc.vertex_func.source = basic_vs_source;
        shd_desc.vertex_func.entry = "main";
        shd_desc.vertex_func.d3d11_target = "vs_5_0";
        shd_desc.vertex_func.d3d12_target = "vs_5_0";
        shd_desc.fragment_func.source = basic_fs_source;
        shd_desc.fragment_func.entry = "main";
        shd_desc.fragment_func.d3d11_target = "ps_5_0";
        shd_desc.fragment_func.d3d12_target = "ps_5_0";
        shd_desc.attrs[0].hlsl_sem_name = "POSITION";
        shd_desc.attrs[1].hlsl_sem_name = "TEXCOORD";
        shd_desc.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
        shd_desc.uniform_blocks[0].size = sizeof(vs_params_t);
        shd_desc.uniform_blocks[0].hlsl_register_b_n = 0;
        shd_desc.uniform_blocks[1].stage = SG_SHADERSTAGE_FRAGMENT;
        shd_desc.uniform_blocks[1].size = sizeof(fs_params_t);
        shd_desc.uniform_blocks[1].hlsl_register_b_n = 0;
        shd_desc.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
        shd_desc.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
        shd_desc.views[0].texture.hlsl_register_t_n = 0;
        shd_desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
        shd_desc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
        shd_desc.samplers[0].hlsl_register_s_n = 0;
        shd_desc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
        shd_desc.texture_sampler_pairs[0].view_slot = 0;
        shd_desc.texture_sampler_pairs[0].sampler_slot = 0;
        char label[64];
        sprintf_s(label, "shader-%d", p);
        shd_desc.label = label;
        g_resources.shaders[p] = sg_make_shader(&shd_desc);

        sg_pipeline_desc pip_desc = {};
        pip_desc.shader = g_resources.shaders[p];
        pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4;
        pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
        pip_desc.index_type = SG_INDEXTYPE_UINT16;
        pip_desc.color_count = 1;
        pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        pip_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;

        // vary blend mode across pipelines
        if (p % 3 == 1)
        {
            pip_desc.colors[0].blend.enabled = true;
            pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        }
        else if (p % 3 == 2)
        {
            pip_desc.colors[0].blend.enabled = true;
            pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
        }

        sprintf_s(label, "pipeline-%d", p);
        pip_desc.label = label;
        g_resources.pipelines[p] = sg_make_pipeline(&pip_desc);
    }

    // create dynamic buffers
    g_resources.dynamic_buffers.resize(g_config.num_buffer_updates);
    for (int b = 0; b < g_config.num_buffer_updates; b++)
    {
        sg_buffer_desc buf_desc = {};
        buf_desc.size = 1024;  // 1KB each
        buf_desc.usage.dynamic_update = true;
        char label[64];
        sprintf_s(label, "dynamic-buffer-%d", b);
        buf_desc.label = label;
        g_resources.dynamic_buffers[b] = sg_make_buffer(&buf_desc);
    }

    // create offscreen render targets (fixed pool of 10)
    const int num_render_targets = 10;
    g_resources.render_targets.resize(num_render_targets);
    g_resources.rt_color_views.resize(num_render_targets);
    g_resources.rt_texture_views.resize(num_render_targets);

    for (int rt = 0; rt < num_render_targets; rt++)
    {
        sg_image_desc img_desc = {};
        img_desc.type = SG_IMAGETYPE_2D;
        img_desc.width = 256;
        img_desc.height = 256;
        img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        img_desc.usage.color_attachment = true;
        char label[64];
        sprintf_s(label, "render-target-%d", rt);
        img_desc.label = label;
        g_resources.render_targets[rt] = sg_make_image(&img_desc);

        sg_view_desc color_view_desc = {};
        color_view_desc.color_attachment.image = g_resources.render_targets[rt];
        sprintf_s(label, "rt-color-view-%d", rt);
        color_view_desc.label = label;
        g_resources.rt_color_views[rt] = sg_make_view(&color_view_desc);

        sg_view_desc tex_view_desc = {};
        tex_view_desc.texture.image = g_resources.render_targets[rt];
        sprintf_s(label, "rt-texture-view-%d", rt);
        tex_view_desc.label = label;
        g_resources.rt_texture_views[rt] = sg_make_view(&tex_view_desc);
    }

    // create compute resources
    g_resources.compute_buffers.resize(g_config.num_compute_dispatches);
    g_resources.compute_views.resize(g_config.num_compute_dispatches);

    for (int c = 0; c < g_config.num_compute_dispatches; c++)
    {
        sg_buffer_desc buf_desc = {};
        buf_desc.size = 4096;  // 4KB storage buffer
        buf_desc.usage.storage_buffer = true;
        char label[64];
        sprintf_s(label, "compute-buffer-%d", c);
        buf_desc.label = label;
        g_resources.compute_buffers[c] = sg_make_buffer(&buf_desc);

        sg_view_desc view_desc = {};
        view_desc.storage_buffer.buffer = g_resources.compute_buffers[c];
        sprintf_s(label, "compute-view-%d", c);
        view_desc.label = label;
        g_resources.compute_views[c] = sg_make_view(&view_desc);
    }

    // compute shader
    sg_shader_desc compute_shd_desc = {};
    compute_shd_desc.compute_func.source = compute_cs_source;
    compute_shd_desc.compute_func.entry = "main";
    compute_shd_desc.compute_func.d3d11_target = "cs_5_0";
    compute_shd_desc.compute_func.d3d12_target = "cs_5_0";
    compute_shd_desc.uniform_blocks[0].stage = SG_SHADERSTAGE_COMPUTE;
    compute_shd_desc.uniform_blocks[0].size = sizeof(cs_params_t);
    compute_shd_desc.uniform_blocks[0].hlsl_register_b_n = 0;
    compute_shd_desc.views[0].storage_buffer.stage = SG_SHADERSTAGE_COMPUTE;
    compute_shd_desc.views[0].storage_buffer.readonly = false;
    compute_shd_desc.views[0].storage_buffer.hlsl_register_u_n = 0;
    compute_shd_desc.label = "compute-shader";
    g_resources.compute_shader = sg_make_shader(&compute_shd_desc);

    sg_pipeline_desc compute_pip_desc = {};
    compute_pip_desc.shader = g_resources.compute_shader;
    compute_pip_desc.compute = true;
    compute_pip_desc.label = "compute-pipeline";
    g_resources.compute_pipeline = sg_make_pipeline(&compute_pip_desc);

    // create instanced resources
    sg_shader_desc instanced_shd_desc = {};
    instanced_shd_desc.vertex_func.source = instanced_vs_source;
    instanced_shd_desc.vertex_func.entry = "main";
    instanced_shd_desc.vertex_func.d3d11_target = "vs_5_0";
    instanced_shd_desc.vertex_func.d3d12_target = "vs_5_0";
    instanced_shd_desc.fragment_func.source = instanced_fs_source;
    instanced_shd_desc.fragment_func.entry = "main";
    instanced_shd_desc.fragment_func.d3d11_target = "ps_5_0";
    instanced_shd_desc.fragment_func.d3d12_target = "ps_5_0";
    instanced_shd_desc.attrs[0].hlsl_sem_name = "POSITION";
    instanced_shd_desc.attrs[0].hlsl_sem_index = 0;
    instanced_shd_desc.attrs[1].hlsl_sem_name = "TEXCOORD";
    instanced_shd_desc.attrs[1].hlsl_sem_index = 0;
    instanced_shd_desc.attrs[2].hlsl_sem_name = "TEXCOORD";
    instanced_shd_desc.attrs[2].hlsl_sem_index = 1;
    instanced_shd_desc.attrs[3].hlsl_sem_name = "TEXCOORD";
    instanced_shd_desc.attrs[3].hlsl_sem_index = 2;
    instanced_shd_desc.attrs[4].hlsl_sem_name = "TEXCOORD";
    instanced_shd_desc.attrs[4].hlsl_sem_index = 3;
    instanced_shd_desc.attrs[5].hlsl_sem_name = "TEXCOORD";
    instanced_shd_desc.attrs[5].hlsl_sem_index = 4;
    instanced_shd_desc.label = "instanced-shader";
    g_resources.instanced_shader = sg_make_shader(&instanced_shd_desc);

    sg_pipeline_desc instanced_pip_desc = {};
    instanced_pip_desc.shader = g_resources.instanced_shader;
    // buffer 0: per-vertex data (position)
    instanced_pip_desc.layout.buffers[0].stride = sizeof(float) * 2;
    instanced_pip_desc.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_VERTEX;
    // buffer 1: per-instance data (matrix + color)
    instanced_pip_desc.layout.buffers[1].stride = sizeof(InstanceData);
    instanced_pip_desc.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
    instanced_pip_desc.layout.attrs[0].buffer_index = 0;
    instanced_pip_desc.layout.attrs[0].offset = 0;
    instanced_pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    instanced_pip_desc.layout.attrs[1].buffer_index = 1;
    instanced_pip_desc.layout.attrs[1].offset = 0;
    instanced_pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4;
    instanced_pip_desc.layout.attrs[2].buffer_index = 1;
    instanced_pip_desc.layout.attrs[2].offset = 16;
    instanced_pip_desc.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT4;
    instanced_pip_desc.layout.attrs[3].buffer_index = 1;
    instanced_pip_desc.layout.attrs[3].offset = 32;
    instanced_pip_desc.layout.attrs[3].format = SG_VERTEXFORMAT_FLOAT4;
    instanced_pip_desc.layout.attrs[4].buffer_index = 1;
    instanced_pip_desc.layout.attrs[4].offset = 48;
    instanced_pip_desc.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT4;
    instanced_pip_desc.layout.attrs[5].buffer_index = 1;
    instanced_pip_desc.layout.attrs[5].offset = 64;
    instanced_pip_desc.layout.attrs[5].format = SG_VERTEXFORMAT_FLOAT4;
    instanced_pip_desc.index_type = SG_INDEXTYPE_UINT16;
    instanced_pip_desc.color_count = 1;
    instanced_pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    instanced_pip_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    instanced_pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    instanced_pip_desc.depth.write_enabled = true;
    instanced_pip_desc.cull_mode = SG_CULLMODE_BACK;
    instanced_pip_desc.label = "instanced-pipeline";
    g_resources.instanced_pipeline = sg_make_pipeline(&instanced_pip_desc);

    // create instance buffer sized to actual needs (with min 1000 for safety)
    int max_instances = (g_config.num_instanced_draws > 1000) ? g_config.num_instanced_draws : 1000;
    sg_buffer_desc inst_buf_desc = {};
    inst_buf_desc.size = sizeof(InstanceData) * max_instances;
    inst_buf_desc.usage.stream_update = true;
    inst_buf_desc.label = "instance-buffer";
    g_resources.instance_buffer = sg_make_buffer(&inst_buf_desc);

    // allocate CPU-side instance data
    g_resources.instance_data.resize(max_instances);

    // create batched shader
    sg_shader_desc batched_shd_desc = {};
    batched_shd_desc.vertex_func.source = batched_vs_source;
    batched_shd_desc.fragment_func.source = batched_fs_source;
    batched_shd_desc.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
    batched_shd_desc.uniform_blocks[0].size = sizeof(DrawIdParams);
    batched_shd_desc.uniform_blocks[0].hlsl_register_b_n = 0;
    batched_shd_desc.views[0].storage_buffer.stage = SG_SHADERSTAGE_VERTEX;
    batched_shd_desc.views[0].storage_buffer.readonly = true;
    batched_shd_desc.views[0].storage_buffer.hlsl_register_t_n = 0;
    batched_shd_desc.views[1].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    batched_shd_desc.views[1].texture.image_type = SG_IMAGETYPE_2D;
    batched_shd_desc.views[1].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    batched_shd_desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    batched_shd_desc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    batched_shd_desc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    batched_shd_desc.texture_sampler_pairs[0].view_slot = 1;
    batched_shd_desc.texture_sampler_pairs[0].sampler_slot = 0;
    batched_shd_desc.attrs[0].hlsl_sem_name = "POSITION";
    batched_shd_desc.attrs[0].hlsl_sem_index = 0;
    batched_shd_desc.attrs[1].hlsl_sem_name = "TEXCOORD";
    batched_shd_desc.attrs[1].hlsl_sem_index = 0;
    batched_shd_desc.label = "batched-shader";
    g_resources.batched_shader = sg_make_shader(&batched_shd_desc);

    sg_pipeline_desc batched_pip_desc = {};
    batched_pip_desc.shader = g_resources.batched_shader;
    batched_pip_desc.layout.buffers[0].stride = sizeof(float) * 6;
    batched_pip_desc.layout.attrs[0].buffer_index = 0;
    batched_pip_desc.layout.attrs[0].offset = 0;
    batched_pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4;
    batched_pip_desc.layout.attrs[1].buffer_index = 0;
    batched_pip_desc.layout.attrs[1].offset = 16;
    batched_pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    batched_pip_desc.index_type = SG_INDEXTYPE_UINT16;
    batched_pip_desc.color_count = 1;
    batched_pip_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    batched_pip_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    batched_pip_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    batched_pip_desc.depth.write_enabled = true;
    batched_pip_desc.cull_mode = SG_CULLMODE_BACK;
    batched_pip_desc.label = "batched-pipeline";
    g_resources.batched_pipeline = sg_make_pipeline(&batched_pip_desc);

    // create transform storage buffer sized to actual needs (with min 1000 for safety)
    int max_transforms = (g_config.num_batched_draws > 1000) ? g_config.num_batched_draws : 1000;
    sg_buffer_desc transform_buf_desc = {};
    transform_buf_desc.size = sizeof(TransformData) * max_transforms;
    transform_buf_desc.usage.storage_buffer = true;
    transform_buf_desc.usage.stream_update = true;
    transform_buf_desc.label = "transform-buffer";
    g_resources.transform_buffer = sg_make_buffer(&transform_buf_desc);

    // create storage buffer view
    sg_view_desc transform_view_desc = {};
    transform_view_desc.storage_buffer.buffer = g_resources.transform_buffer;
    transform_view_desc.storage_buffer.offset = 0;
    transform_view_desc.label = "transform-view";
    g_resources.transform_view = sg_make_view(&transform_view_desc);

    // allocate CPU-side transform data
    g_resources.transform_data.resize(max_transforms);

    // create object instances (fixed pool of 500)
    const int num_objects = 500;
    g_resources.objects.resize(num_objects);

    for (int i = 0; i < num_objects; i++)
    {
        STRESS_RESOURCES::OBJECT& obj = g_resources.objects[i];

        // arrange in grid
        int grid_size = (int)sqrtf((float)num_objects) + 1;
        float spacing = 2.0f / grid_size;
        obj.x = -1.0f + spacing * 0.5f + spacing * (i % grid_size);
        obj.y = -1.0f + spacing * 0.5f + spacing * (i / grid_size);

        obj.scale = 0.4f;  // perfect size
        obj.rotation = (float)i * 0.1f;
        obj.rotation_speed = 0.5f + (float)(i % 10) * 0.1f;
        obj.texture_index = i % g_config.num_textures;
        obj.pipeline_index = i % g_config.num_pipelines;

        // color tint
        obj.tint[0] = 0.7f + 0.3f * sinf(i * 0.1f);
        obj.tint[1] = 0.7f + 0.3f * sinf(i * 0.1f + 2.094f);
        obj.tint[2] = 0.7f + 0.3f * sinf(i * 0.1f + 4.189f);
        obj.tint[3] = 1.0f;
    }

    log_output("Stress test resources created successfully\n");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

static void destroy_stress_resources()
{
    for (auto& obj : g_resources.objects) { (void)obj; }
    g_resources.objects.clear();

    sg_destroy_pipeline(g_resources.compute_pipeline);
    sg_destroy_shader(g_resources.compute_shader);
    for (auto& view : g_resources.compute_views) sg_destroy_view(view);
    for (auto& buf : g_resources.compute_buffers) sg_destroy_buffer(buf);

    for (auto& view : g_resources.rt_texture_views) sg_destroy_view(view);
    for (auto& view : g_resources.rt_color_views) sg_destroy_view(view);
    for (auto& rt : g_resources.render_targets) sg_destroy_image(rt);

    for (auto& buf : g_resources.dynamic_buffers) sg_destroy_buffer(buf);

    for (auto& pip : g_resources.pipelines) sg_destroy_pipeline(pip);
    for (auto& shd : g_resources.shaders) sg_destroy_shader(shd);

    for (auto& smp : g_resources.samplers) sg_destroy_sampler(smp);
    for (auto& view : g_resources.texture_views) sg_destroy_view(view);
    for (auto& tex : g_resources.textures) sg_destroy_image(tex);

    sg_destroy_buffer(g_resources.quad_ibuf);
    sg_destroy_buffer(g_resources.quad_vbuf);
}

// ---------------------------------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // parse command line arguments
    bool enable_logging = false;
    const char* log_filename = "stress_test.log";
    int auto_exit_seconds = 0;  // 0 = don't auto-exit

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-log") == 0 || strcmp(argv[i], "--log") == 0)
        {
            enable_logging = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                log_filename = argv[++i];
            }
        }
        else if (strcmp(argv[i], "-duration") == 0 || strcmp(argv[i], "--duration") == 0)
        {
            if (i + 1 < argc)
            {
                auto_exit_seconds = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-draws") == 0 || strcmp(argv[i], "--draws") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_draws = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-instanced-draws") == 0 || strcmp(argv[i], "--instanced-draws") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_instanced_draws = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-batched-draws") == 0 || strcmp(argv[i], "--batched-draws") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_batched_draws = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-state-changes") == 0 || strcmp(argv[i], "--state-changes") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_state_changes = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-buffer-updates") == 0 || strcmp(argv[i], "--buffer-updates") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_buffer_updates = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-texture-updates") == 0 || strcmp(argv[i], "--texture-updates") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_texture_updates = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-passes") == 0 || strcmp(argv[i], "--passes") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_passes = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-compute") == 0 || strcmp(argv[i], "--compute") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_compute_dispatches = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-pipelines") == 0 || strcmp(argv[i], "--pipelines") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_pipelines = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-textures") == 0 || strcmp(argv[i], "--textures") == 0)
        {
            if (i + 1 < argc)
            {
                g_config.num_textures = atoi(argv[++i]);
            }
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printf("Usage: %s [options]\n", argv[0]);
            printf("\nTest Operations (per frame):\n");
            printf("  -draws <count>            Number of draw calls (default: 0)\n");
            printf("  -instanced-draws <count>  Number of instances in ONE draw call (default: 0)\n");
            printf("  -batched-draws <count>    Number of draws with batched uniforms (default: 0)\n");
            printf("  -state-changes <count>    Number of pipeline/binding changes (default: 0)\n");
            printf("  -buffer-updates <count>   Number of dynamic buffer updates (default: 0)\n");
            printf("  -texture-updates <count>  Number of texture updates (default: 0)\n");
            printf("  -passes <count>           Number of render passes (default: 1)\n");
            printf("  -compute <count>          Number of compute dispatches (default: 0, D3D12 only)\n");
            printf("\nResource Pools:\n");
            printf("  -pipelines <count>        Number of pipelines to create (default: 10)\n");
            printf("  -textures <count>         Number of textures to create (default: 25)\n");
            printf("\nOther Options:\n");
            printf("  -log [filename]           Write output to log file (default: stress_test.log)\n");
            printf("  -duration <seconds>       Auto-exit after specified seconds (for unattended testing)\n");
            printf("  -h, --help                Show this help\n");
            printf("\nExamples:\n");
            printf("  %s -draws 30                        # Test with 30 draw calls\n", argv[0]);
            printf("  %s -draws 100 -buffer-updates 50    # 100 draws + 50 buffer updates\n", argv[0]);
            printf("  %s -state-changes 25 -pipelines 10  # 25 state changes across 10 pipelines\n", argv[0]);
            return 0;
        }
    }

    // open log file if requested
    if (enable_logging)
    {
        fopen_s(&g_log_file, log_filename, "w");
        if (g_log_file)
        {
            printf("Logging to: %s\n", log_filename);
        }
        else
        {
            printf("Warning: Failed to open log file: %s\n", log_filename);
        }
    }

#if defined(SOKOL_D3D12)
    log_output("D3D12 Stress Test\n");
#else
    log_output("D3D11 Stress Test\n");
#endif
    log_output("=================\n\n");

    if (auto_exit_seconds > 0)
    {
        log_output("Auto-exit enabled: will run for %d seconds\n\n", auto_exit_seconds);
    }

    // create window
    if (!create_window())
    {
        return 1;
    }

    // create device
    if (!create_device())
    {
        return 1;
    }

    // create render targets
    if (!create_render_targets())
    {
        return 1;
    }

    // setup sokol gfx
    sg_desc desc = {};

    // increase pool sizes for extreme stress tests
    desc.buffer_pool_size = 4096;       // default 128, need up to 2000+ for Tier 3
    desc.image_pool_size = 512;         // default 128, need more for textures
    desc.sampler_pool_size = 256;       // default 64
    desc.shader_pool_size = 128;        // default 32, need more for pipelines
    desc.pipeline_pool_size = 256;      // default 64, need more pipelines
    desc.view_pool_size = 4096;         // default 256, need views for buffers

    desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    desc.environment.defaults.sample_count = 1;
#if defined(SOKOL_D3D12)
    desc.environment.d3d12.device = g_state.device;
    desc.environment.d3d12.command_queue = g_state.command_queue;
    desc.environment.d3d12.fence = g_state.fence;
    desc.environment.d3d12.fence_event = g_state.fence_event;
    desc.environment.d3d12.fence_value = &g_state.next_fence_value;
#else
    desc.environment.d3d11.device = g_state.device;
    desc.environment.d3d11.device_context = g_state.context;
#endif
    desc.logger.func = []([[maybe_unused]] const char* tag, uint32_t log_level, uint32_t log_item_id,
                          const char* message, [[maybe_unused]] uint32_t line_nr, [[maybe_unused]] const char* filename,
                          [[maybe_unused]] void* user_data) {
        const char* level_str = "???";
        switch (log_level)
        {
        case 0:
            level_str = "PANIC";
            break;
        case 1:
            level_str = "ERROR";
            break;
        case 2:
            level_str = "WARN";
            break;
        case 3:
            level_str = "INFO";
            break;
        }
        if (message)
        {
            printf("[sokol][%s] %s\n", level_str, message);
        }
        else
        {
            printf("[sokol][%s] error_id=%u\n", level_str, log_item_id);
        }
        fflush(stdout);
    };

    sg_setup(&desc);
    if (!sg_isvalid())
    {
        log_output("Error: sokol_gfx initialization failed\n");
        return 1;
    }

#if defined(SOKOL_D3D12)
    log_output("Sokol GFX initialized with D3D12 backend\n\n");
#else
    log_output("Sokol GFX initialized with D3D11 backend\n\n");
#endif

    // print initial configuration
    update_stress_config();

    // create stress test resources
    if (!create_stress_resources())
    {
        return 1;
    }

    log_output("Starting test... (press ESC to exit)\n\n");

    // initialize metrics
    g_metrics.frame_times.resize(60, 0.0f);

    // main loop
    float time = 0.0f;
    int frame_count = 0;
    auto last_frame_time = std::chrono::high_resolution_clock::now();
    auto start_time = std::chrono::high_resolution_clock::now();

    while (g_state.running)
    {
        // check auto-exit duration
        if (auto_exit_seconds > 0)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::high_resolution_clock::now() - start_time).count();
            if (elapsed >= auto_exit_seconds)
            {
                // calculate average FPS over entire test duration
                float average_fps = (elapsed > 0) ? (float)frame_count / (float)elapsed : 0.0f;
                log_output("\nAuto-exit: %d seconds elapsed\n", auto_exit_seconds);
                log_output("=== TEST COMPLETE ===\n");
                log_output("Total Frames: %d\n", frame_count);
                log_output("Total Time: %d seconds\n", elapsed);
                log_output("AVERAGE FPS: %.1f\n", average_fps);
                log_output("=====================\n\n");
                g_state.running = false;
                break;
            }
        }

        // measure frame time
        auto current_time = std::chrono::high_resolution_clock::now();
        float frame_time = std::chrono::duration<float, std::milli>(current_time - last_frame_time).count();
        last_frame_time = current_time;

        // update rolling average
        g_metrics.frame_times[g_metrics.frame_index] = frame_time;
        g_metrics.frame_index = (g_metrics.frame_index + 1) % 60;

        float avg_frame_time = 0.0f;
        for (float ft : g_metrics.frame_times)
        {
            avg_frame_time += ft;
        }
        avg_frame_time /= 60.0f;

        g_metrics.frame_time_ms = avg_frame_time;
        g_metrics.fps = 1000.0f / avg_frame_time;

        // reset counters
        g_metrics.draw_calls = 0;
        g_metrics.state_changes = 0;
        g_metrics.buffer_updates = 0;
        g_metrics.texture_updates = 0;
        g_metrics.compute_dispatches = 0;

        // process messages
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_state.running)
        {
            break;
        }

        // handle resize
        if (g_state.resized && frame_count > 3)
        {
            g_state.resized = false;
            if (!resize_render_targets())
            {
                log_output("Error: failed to resize render targets\n");
                break;
            }
        }

        time += 0.016f;
        frame_count++;

        // -------------------------------------------------------------------------
        // BUFFER UPDATES
        // -------------------------------------------------------------------------
        for (int b = 0; b < g_config.num_buffer_updates; b++)
        {
            uint8_t data[1024];
            for (int i = 0; i < 1024; i++)
            {
                data[i] = (uint8_t)((i + frame_count + b) & 0xFF);
            }
            sg_update_buffer(g_resources.dynamic_buffers[b], SG_RANGE(data));
            g_metrics.buffer_updates++;
        }

        // -------------------------------------------------------------------------
        // TEXTURE UPDATES
        // -------------------------------------------------------------------------
        constexpr int TEX_SIZE = 64;
        uint32_t pixels[TEX_SIZE * TEX_SIZE];

        for (int t = 0; t < std::min(g_config.num_texture_updates, g_config.num_textures); t++)
        {
            // generate animated pattern
            for (int y = 0; y < TEX_SIZE; y++)
            {
                for (int x = 0; x < TEX_SIZE; x++)
                {
                    float fx = (float)x / TEX_SIZE;
                    float fy = (float)y / TEX_SIZE;
                    float v = sinf(fx * 10.0f + time + t) * sinf(fy * 10.0f + time + t);
                    v = v * 0.5f + 0.5f;

                    uint8_t c = (uint8_t)(v * 255);
                    pixels[y * TEX_SIZE + x] = (255 << 24) | (c << 16) | (c << 8) | c;
                }
            }

            sg_image_data img_data = {};
            img_data.mip_levels[0].ptr = pixels;
            img_data.mip_levels[0].size = sizeof(pixels);
            sg_update_image(g_resources.textures[t], &img_data);
            g_metrics.texture_updates++;
        }

        // -------------------------------------------------------------------------
        // COMPUTE PASSES
        // -------------------------------------------------------------------------
        for (int c = 0; c < g_config.num_compute_dispatches; c++)
        {
            sg_pass compute_pass = {};
            compute_pass.compute = true;
            sg_begin_pass(&compute_pass);

            sg_apply_pipeline(g_resources.compute_pipeline);

            sg_bindings compute_bind = {};
            compute_bind.views[0] = g_resources.compute_views[c];
            sg_apply_bindings(&compute_bind);

            cs_params_t cs_params;
            cs_params.time = time + c;
            cs_params._pad[0] = 0.0f;
            cs_params._pad[1] = 0.0f;
            cs_params._pad[2] = 0.0f;
            sg_apply_uniforms(0, SG_RANGE(cs_params));

            sg_dispatch(1, 1, 1);

            sg_end_pass();
            g_metrics.compute_dispatches++;
        }

        // -------------------------------------------------------------------------
        // OFFSCREEN PASSES
        // -------------------------------------------------------------------------
        // num_passes includes the main pass, so offscreen passes = num_passes - 1
        int num_offscreen_passes = (g_config.num_passes > 1) ? (g_config.num_passes - 1) : 0;
        for (int rt = 0; rt < num_offscreen_passes; rt++)
        {
            sg_pass offscreen_pass = {};
            offscreen_pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            offscreen_pass.action.colors[0].clear_value = {0.1f, 0.1f, 0.2f, 1.0f};
            offscreen_pass.attachments.colors[0] = g_resources.rt_color_views[rt];

            sg_begin_pass(&offscreen_pass);

            // draw a few objects to each render target
            int objects_per_rt = 10;
            for (int i = 0; i < objects_per_rt; i++)
            {
                int obj_idx = (rt * objects_per_rt + i) % 500;
                const STRESS_RESOURCES::OBJECT& obj = g_resources.objects[obj_idx];

                sg_apply_pipeline(g_resources.pipelines[obj.pipeline_index]);
                g_metrics.state_changes++;

                sg_bindings bind = {};
                bind.vertex_buffers[0] = g_resources.quad_vbuf;
                bind.index_buffer = g_resources.quad_ibuf;
                bind.views[0] = g_resources.texture_views[obj.texture_index];
                bind.samplers[0] = g_resources.samplers[obj_idx % 4];
                sg_apply_bindings(&bind);

                vs_params_t vs_params;
                float trans[16], scale[16], rot[16], temp[16];

                mat4_translate(trans, obj.x, obj.y, 0.0f);
                mat4_scale(scale, obj.scale, obj.scale, 1.0f);
                mat4_rotate_z(rot, obj.rotation + time * obj.rotation_speed);
                mat4_multiply(temp, scale, rot);
                mat4_multiply(vs_params.mvp, trans, temp);
                sg_apply_uniforms(0, SG_RANGE(vs_params));

                fs_params_t fs_params;
                memcpy(fs_params.tint, obj.tint, sizeof(fs_params.tint));
                sg_apply_uniforms(1, SG_RANGE(fs_params));

                sg_draw(0, 6, 1);
                g_metrics.draw_calls++;
            }

            sg_end_pass();
        }

        // -------------------------------------------------------------------------
        // PREPARE INSTANCE DATA
        // -------------------------------------------------------------------------
        if (g_config.num_instanced_draws > 0)
        {
            for (int i = 0; i < g_config.num_instanced_draws; i++)
            {
                const STRESS_RESOURCES::OBJECT& obj = g_resources.objects[i % 500];

                float trans[16], scale[16], rot[16], temp[16];
                mat4_translate(trans, obj.x, obj.y, 0.0f);
                mat4_scale(scale, obj.scale, obj.scale, 1.0f);
                mat4_rotate_z(rot, obj.rotation + time * obj.rotation_speed);
                mat4_multiply(temp, scale, rot);
                mat4_multiply(g_resources.instance_data[i].mvp, trans, temp);

                memcpy(g_resources.instance_data[i].tint, obj.tint, sizeof(float) * 4);
            }

            // upload to GPU
            sg_range inst_range;
            inst_range.ptr = g_resources.instance_data.data();
            inst_range.size = sizeof(InstanceData) * g_config.num_instanced_draws;
            sg_update_buffer(g_resources.instance_buffer, &inst_range);
        }

        // -------------------------------------------------------------------------
        // PREPARE BATCHED TRANSFORM DATA
        // -------------------------------------------------------------------------
        if (g_config.num_batched_draws > 0)
        {
            for (int i = 0; i < g_config.num_batched_draws; i++)
            {
                const STRESS_RESOURCES::OBJECT& obj = g_resources.objects[i % 500];

                float trans[16], scale[16], rot[16], temp[16];
                mat4_translate(trans, obj.x, obj.y, 0.0f);
                mat4_scale(scale, obj.scale, obj.scale, 1.0f);
                mat4_rotate_z(rot, obj.rotation + time * obj.rotation_speed);
                mat4_multiply(temp, scale, rot);
                mat4_multiply(g_resources.transform_data[i].mvp, trans, temp);

                memcpy(g_resources.transform_data[i].tint, obj.tint, sizeof(float) * 4);
            }

            // upload to GPU
            sg_range transform_range;
            transform_range.ptr = g_resources.transform_data.data();
            transform_range.size = sizeof(TransformData) * g_config.num_batched_draws;
            sg_update_buffer(g_resources.transform_buffer, &transform_range);
        }

        // -------------------------------------------------------------------------
        // SWAPCHAIN PASS (main rendering)
        // -------------------------------------------------------------------------
        // Distribute draws across multiple main passes if num_passes > 1
        int draws_per_main_pass = g_config.num_draws;
        int num_main_passes = 1;
        if (g_config.num_passes > 1 && g_config.num_draws > 0)
        {
            num_main_passes = g_config.num_passes;
            draws_per_main_pass = g_config.num_draws / num_main_passes;
        }

        for (int pass_idx = 0; pass_idx < num_main_passes; pass_idx++)
        {
            // setup swapchain for sokol
            sg_swapchain swapchain = {};
            swapchain.width = (int)g_state.width;
            swapchain.height = (int)g_state.height;
            swapchain.sample_count = 1;
            swapchain.color_format = SG_PIXELFORMAT_RGBA8;
            swapchain.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;

#if defined(SOKOL_D3D12)
            // get current render target handles
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_state.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv_handle.ptr += g_state.frame_index * g_state.rtv_descriptor_size;
            D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = g_state.dsv_heap->GetCPUDescriptorHandleForHeapStart();

            swapchain.d3d12.render_view = (const void*)rtv_handle.ptr;
            swapchain.d3d12.depth_stencil_view = (const void*)dsv_handle.ptr;
            swapchain.d3d12.render_target = g_state.render_targets[g_state.frame_index];
            swapchain.d3d12.depth_stencil = g_state.depth_stencil;
#else
            swapchain.d3d11.render_view = g_state.rtv;
            swapchain.d3d11.depth_stencil_view = g_state.dsv;
#endif

            sg_pass pass = {};
            // Clear on first pass, load on subsequent passes
            pass.action.colors[0].load_action = (pass_idx == 0) ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
            pass.action.colors[0].clear_value = {0.05f, 0.05f, 0.1f, 1.0f};
            pass.action.depth.load_action = (pass_idx == 0) ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
            pass.action.depth.clear_value = 1.0f;
            pass.swapchain = swapchain;

            sg_begin_pass(&pass);

            // ----------------------------------------------------------------
            // DRAWS with state changes
            // ----------------------------------------------------------------
            if (g_config.num_state_changes > 0)
            {
                // change pipeline/bindings for each draw
                for (int i = 0; i < g_config.num_state_changes; i++)
                {
                    const STRESS_RESOURCES::OBJECT& obj = g_resources.objects[i % 500];

                    sg_apply_pipeline(g_resources.pipelines[i % g_config.num_pipelines]);
                    g_metrics.state_changes++;

                    sg_bindings bind = {};
                    bind.vertex_buffers[0] = g_resources.quad_vbuf;
                    bind.index_buffer = g_resources.quad_ibuf;
                    bind.views[0] = g_resources.texture_views[i % g_config.num_textures];
                    bind.samplers[0] = g_resources.samplers[i % 4];
                    sg_apply_bindings(&bind);

                    vs_params_t vs_params;
                    float trans[16], scale[16], rot[16], temp[16];

                    mat4_translate(trans, obj.x, obj.y, 0.0f);
                    mat4_scale(scale, obj.scale, obj.scale, 1.0f);
                    mat4_rotate_z(rot, obj.rotation + time * obj.rotation_speed);
                    mat4_multiply(temp, scale, rot);
                    mat4_multiply(vs_params.mvp, trans, temp);
                    sg_apply_uniforms(0, SG_RANGE(vs_params));

                    fs_params_t fs_params;
                    memcpy(fs_params.tint, obj.tint, sizeof(fs_params.tint));
                    sg_apply_uniforms(1, SG_RANGE(fs_params));

                    sg_draw(0, 6, 1);
                    g_metrics.draw_calls++;
                }
            }

            // ----------------------------------------------------------------
            // DRAWS without state changes (batched)
            // ----------------------------------------------------------------
            if (draws_per_main_pass > 0)
            {
                // set pipeline and bindings once, then just issue draws
                sg_apply_pipeline(g_resources.pipelines[0]);
                g_metrics.state_changes++;

                sg_bindings bind = {};
                bind.vertex_buffers[0] = g_resources.quad_vbuf;
                bind.index_buffer = g_resources.quad_ibuf;
                bind.views[0] = g_resources.texture_views[0];
                bind.samplers[0] = g_resources.samplers[0];
                sg_apply_bindings(&bind);

                // loop just issuing draws with different uniforms
                int draw_offset = pass_idx * draws_per_main_pass;
                for (int i = 0; i < draws_per_main_pass; i++)
                {
                    const STRESS_RESOURCES::OBJECT& obj = g_resources.objects[(draw_offset + i) % 500];

                    vs_params_t vs_params;
                    float trans[16], scale[16], rot[16], temp[16];

                    mat4_translate(trans, obj.x, obj.y, 0.0f);
                    mat4_scale(scale, obj.scale, obj.scale, 1.0f);
                    mat4_rotate_z(rot, obj.rotation + time * obj.rotation_speed);
                    mat4_multiply(temp, scale, rot);
                    mat4_multiply(vs_params.mvp, trans, temp);
                    sg_apply_uniforms(0, SG_RANGE(vs_params));

                    fs_params_t fs_params;
                    memcpy(fs_params.tint, obj.tint, sizeof(fs_params.tint));
                    sg_apply_uniforms(1, SG_RANGE(fs_params));

                    sg_draw(0, 6, 1);
                    g_metrics.draw_calls++;
                }
            }

            // ----------------------------------------------------------------
            // INSTANCED DRAWS (one draw call for all instances)
            // ----------------------------------------------------------------
            if (g_config.num_instanced_draws > 0)
            {
                sg_apply_pipeline(g_resources.instanced_pipeline);
                g_metrics.state_changes++;

                sg_bindings bind = {};
                bind.vertex_buffers[0] = g_resources.quad_vbuf;
                bind.vertex_buffers[1] = g_resources.instance_buffer;
                bind.index_buffer = g_resources.quad_ibuf;
                sg_apply_bindings(&bind);

                // ONE draw call for ALL instances!
                sg_draw(0, 6, g_config.num_instanced_draws);
                g_metrics.draw_calls++;
            }

            // ----------------------------------------------------------------
            // BATCHED DRAWS (storage buffer with indexed access)
            // ----------------------------------------------------------------
            if (g_config.num_batched_draws > 0)
            {
                sg_apply_pipeline(g_resources.batched_pipeline);
                g_metrics.state_changes++;

                sg_bindings bind = {};
                bind.vertex_buffers[0] = g_resources.quad_vbuf;
                bind.index_buffer = g_resources.quad_ibuf;
                bind.views[0] = g_resources.transform_view;
                bind.views[1] = g_resources.texture_views[0];
                bind.samplers[0] = g_resources.samplers[0];
                sg_apply_bindings(&bind);

                // draw with small uniform updates (just index)
                for (int i = 0; i < g_config.num_batched_draws; i++)
                {
                    DrawIdParams draw_params;
                    draw_params.draw_id = (uint32_t)i;
                    draw_params._pad[0] = 0;
                    draw_params._pad[1] = 0;
                    draw_params._pad[2] = 0;
                    sg_apply_uniforms(0, SG_RANGE(draw_params));

                    sg_draw(0, 6, 1);
                    g_metrics.draw_calls++;
                }
            }

            sg_end_pass();
        }

        // time sg_commit (D3D12: close and execute command list)
        auto t_commit_start = std::chrono::high_resolution_clock::now();
        sg_commit();
        auto t_commit_end = std::chrono::high_resolution_clock::now();

        // time present (DXGI swap chain present)
        UINT present_flags = g_state.tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
        auto t_present_start = std::chrono::high_resolution_clock::now();
        g_state.swap_chain->Present(0, present_flags);
        auto t_present_end = std::chrono::high_resolution_clock::now();

#if defined(SOKOL_D3D12)
        // time wait_for_previous_frame (fence synchronization)
        auto t_wait_start = std::chrono::high_resolution_clock::now();
        wait_for_previous_frame();
        auto t_wait_end = std::chrono::high_resolution_clock::now();

        // accumulate timing (microseconds)
        g_metrics.total_commit_us += std::chrono::duration_cast<std::chrono::microseconds>(t_commit_end - t_commit_start).count();
        g_metrics.total_present_us += std::chrono::duration_cast<std::chrono::microseconds>(t_present_end - t_present_start).count();
        g_metrics.total_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(t_wait_end - t_wait_start).count();
        g_metrics.timing_samples++;
#else
        // D3D11: no fence wait, just accumulate commit and present timing
        g_metrics.total_commit_us += std::chrono::duration_cast<std::chrono::microseconds>(t_commit_end - t_commit_start).count();
        g_metrics.total_present_us += std::chrono::duration_cast<std::chrono::microseconds>(t_present_end - t_present_start).count();
        g_metrics.total_wait_us += 0.0;  // D3D11 has no explicit wait
        g_metrics.timing_samples++;
#endif

        // print metrics every 60 frames (after all rendering is done)
        if (frame_count % 60 == 0)
        {
            log_output("Frame: %6d | FPS: %6.1f | Frame Time: %6.2f ms | Draw Calls: %5d | State Changes: %5d | "
                   "Buffer Updates: %5d | Texture Updates: %3d | Compute: %3d\n",
                   frame_count, g_metrics.fps, g_metrics.frame_time_ms, g_metrics.draw_calls, g_metrics.state_changes,
                   g_metrics.buffer_updates, g_metrics.texture_updates, g_metrics.compute_dispatches);
        }

        // print timing breakdown every 3000 frames
        if (frame_count % 3000 == 0)
        {
            if (g_metrics.timing_samples > 0)
            {
                double avg_commit_us = g_metrics.total_commit_us / g_metrics.timing_samples;
                double avg_present_us = g_metrics.total_present_us / g_metrics.timing_samples;
                double avg_wait_us = g_metrics.total_wait_us / g_metrics.timing_samples;
                log_output("  [TIMING] commit: %6.2f us | present: %6.2f us | wait: %6.2f us (samples: %d)\n",
                       avg_commit_us, avg_present_us, avg_wait_us, g_metrics.timing_samples);
            }
            else
            {
                log_output("  [TIMING] No samples yet (timing_samples = %d)\n", g_metrics.timing_samples);
            }
        }

        // benchmark mode removed - use command line parameters and -duration instead
    }

#if defined(SOKOL_D3D12)
    // print detailed timing breakdown
    if (g_metrics.timing_samples > 0)
    {
        double avg_commit_us = g_metrics.total_commit_us / g_metrics.timing_samples;
        double avg_present_us = g_metrics.total_present_us / g_metrics.timing_samples;
        double avg_wait_us = g_metrics.total_wait_us / g_metrics.timing_samples;
        double total_us = avg_commit_us + avg_present_us + avg_wait_us;

        log_output("\n");
        log_output("========================================\n");
        log_output("D3D12 Timing Breakdown (per frame):\n");
        log_output("========================================\n");
        log_output("sg_commit():             %7.2f us (%5.1f%%)\n", avg_commit_us, (avg_commit_us / total_us) * 100.0);
        log_output("Present():               %7.2f us (%5.1f%%)\n", avg_present_us, (avg_present_us / total_us) * 100.0);
        log_output("wait_for_previous_frame: %7.2f us (%5.1f%%)\n", avg_wait_us, (avg_wait_us / total_us) * 100.0);
        log_output("----------------------------------------\n");
        log_output("Total:                   %7.2f us\n", total_us);
        log_output("========================================\n");
        log_output("\n");
    }
#endif

    // cleanup
    printf("\nShutting down...\n");

#if defined(SOKOL_D3D12)
    wait_for_gpu();
#endif

    destroy_stress_resources();
    sg_shutdown();
    destroy_device();

    log_output("Done.\n");

    // close log file
    if (g_log_file)
    {
        fclose(g_log_file);
        g_log_file = nullptr;
    }

    return 0;
}
