#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>

static FILE *g_report = nullptr;

static void report(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_report) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(g_report, fmt, copy);
        va_end(copy);
        fflush(g_report);
    }
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static UINT env_uint(const wchar_t *name, UINT fallback) {
    wchar_t value[32]{};
    DWORD length = GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(value[0]));
    if (length == 0 || length >= sizeof(value) / sizeof(value[0]))
        return fallback;
    return static_cast<UINT>(wcstoul(value, nullptr, 10));
}

static bool env_enabled(const wchar_t *name) {
    wchar_t value[8]{};
    DWORD length = GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(value[0]));
    return length != 0;
}

int main() {
    g_report = fopen("d3d12_window_swapchain_smoke.result.txt", "a");
    report("phase=begin pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));

    const UINT adapter_wanted = env_uint(L"MGPU_D3D12_ADAPTER_INDEX", 0);
    const bool show_window = env_enabled(L"MGPU_SWAPCHAIN_SHOW");
    const bool official_profile = env_enabled(L"MGPU_SWAPCHAIN_OFFICIAL_PROFILE");
    report("config adapter_index=%u show_window=%s\n", adapter_wanted,
           show_window ? "true" : "false");
    report("config official_profile=%s\n", official_profile ? "true" : "false");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"DLSS5MGPUWindowSwapchainSmoke";
    ATOM atom = RegisterClassExW(&window_class);
    report("phase=register_window hr=0x%08lx atom=%u\n",
           static_cast<unsigned long>(atom ? S_OK : HRESULT_FROM_WIN32(GetLastError())),
           static_cast<unsigned int>(atom));
    if (!atom) return 2;

    HWND hwnd = CreateWindowExW(
        0, window_class.lpszClassName, L"DLSS5 MGPU swapchain smoke",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
        nullptr, nullptr, window_class.hInstance, nullptr);
    report("phase=create_window hwnd=%p error=%lu\n", hwnd,
           static_cast<unsigned long>(hwnd ? ERROR_SUCCESS : GetLastError()));
    if (!hwnd) return 3;
    if (show_window) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = official_profile
        ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))
        : CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    report("phase=create_factory hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 4;

    IDXGIAdapter1 *adapter = nullptr;
    hr = factory->EnumAdapters1(adapter_wanted, &adapter);
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        report("phase=select_adapter hr=0x%08lx name=%ls vendor=0x%04lx luid=0x%016llx\n",
               static_cast<unsigned long>(hr), desc.Description,
               static_cast<unsigned long>(desc.VendorId),
               (static_cast<unsigned long long>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                   static_cast<unsigned long long>(static_cast<uint32_t>(desc.AdapterLuid.LowPart)));
    } else {
        report("phase=select_adapter hr=0x%08lx\n", static_cast<unsigned long>(hr));
    }
    if (FAILED(hr) || !adapter) return 5;

    ID3D12Device *device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                           IID_PPV_ARGS(&device));
    report("phase=create_device hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 6;

    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    report("phase=create_queue hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 7;

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = 640;
    swap_desc.Height = 360;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.Stereo = FALSE;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = official_profile ? 3 : 2;
    swap_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc{};
    fullscreen_desc.Windowed = TRUE;
    fullscreen_desc.RefreshRate.Numerator = 60;
    fullscreen_desc.RefreshRate.Denominator = 1;
    IDXGISwapChain1 *swapchain = nullptr;
    report("phase=create_swapchain_begin\n");
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &swap_desc,
                                         official_profile ? &fullscreen_desc : nullptr,
                                         nullptr, &swapchain);
    report("phase=create_swapchain hr=0x%08lx ptr=%p\n",
           static_cast<unsigned long>(hr), swapchain);
    if (FAILED(hr)) return 8;

    hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    report("phase=window_association hr=0x%08lx\n", static_cast<unsigned long>(hr));

    if (env_enabled(L"MGPU_SWAPCHAIN_NGX")) {
        HMODULE ngx = LoadLibraryW(L"nvngx_dlss.dll");
        FARPROC init = ngx ? GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext") : nullptr;
        FARPROC requirements = ngx ? GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetFeatureRequirements") : nullptr;
        report("phase=ngx_load library=%s init_ext=%s feature_requirements=%s error=%lu\n",
               ngx ? "ok" : "failed", init ? "ok" : "missing",
               requirements ? "ok" : "missing",
               static_cast<unsigned long>(ngx ? ERROR_SUCCESS : GetLastError()));
        if (ngx)
            FreeLibrary(ngx);
    }

    ID3D12Resource *backbuffer = nullptr;
    hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    report("phase=get_backbuffer hr=0x%08lx ptr=%p\n",
           static_cast<unsigned long>(hr), backbuffer);
    if (FAILED(hr)) return 9;
    if (official_profile) {
        for (UINT index = 1; index < swap_desc.BufferCount; ++index) {
            ID3D12Resource *extra = nullptr;
            HRESULT extra_hr = swapchain->GetBuffer(index, IID_PPV_ARGS(&extra));
            report("phase=get_backbuffer index=%u hr=0x%08lx ptr=%p\n", index,
                   static_cast<unsigned long>(extra_hr), extra);
            if (extra) extra->Release();
            if (FAILED(extra_hr)) return 16;
        }
    }

    ID3D12DescriptorHeap *rtv_heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_start{};
    UINT rtv_increment = 0;
    if (official_profile) {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.NumDescriptors = swap_desc.BufferCount;
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap));
        report("phase=create_rtv_heap hr=0x%08lx ptr=%p\n",
               static_cast<unsigned long>(hr), rtv_heap);
        if (FAILED(hr)) return 17;
        rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT index = 0; index < swap_desc.BufferCount; ++index) {
            ID3D12Resource *target = nullptr;
            HRESULT target_hr = swapchain->GetBuffer(index, IID_PPV_ARGS(&target));
            if (SUCCEEDED(target_hr) && target) {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_start;
                handle.ptr += static_cast<SIZE_T>(index) * rtv_increment;
                device->CreateRenderTargetView(target, nullptr, handle);
                target->Release();
            }
            report("phase=create_rtv index=%u get_buffer_hr=0x%08lx\n", index,
                   static_cast<unsigned long>(target_hr));
            if (FAILED(target_hr)) return 18;
        }
    }

    ID3D12CommandAllocator *allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&allocator));
    report("phase=create_allocator hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 10;

    ID3D12GraphicsCommandList *list = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                   nullptr, IID_PPV_ARGS(&list));
    report("phase=create_command_list hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 11;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    if (official_profile && rtv_heap) {
        list->OMSetRenderTargets(1, &rtv_start, FALSE, nullptr);
        const FLOAT clear_color[4] = {0.03f, 0.07f, 0.11f, 1.0f};
        list->ClearRenderTargetView(rtv_start, clear_color, 0, nullptr);
        D3D12_RESOURCE_BARRIER present_barrier = barrier;
        present_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        present_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &present_barrier);
        report("phase=record_rtv_clear success=true\n");
    }
    hr = list->Close();
    report("phase=close_command_list hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 12;

    ID3D12Fence *fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    report("phase=create_fence hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 13;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 14;

    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(fence, 1);
    report("phase=execute_signal hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        if (fence->GetCompletedValue() < 1) {
            hr = fence->SetEventOnCompletion(1, event);
            report("phase=wait_arm hr=0x%08lx\n", static_cast<unsigned long>(hr));
            if (SUCCEEDED(hr)) {
                DWORD wait_result = WaitForSingleObject(event, 5000);
                report("phase=wait_result value=%lu\n",
                       static_cast<unsigned long>(wait_result));
            }
        } else {
            report("phase=wait_result value=already_complete\n");
        }
    }

    report("phase=present_begin\n");
    hr = swapchain->Present(0, 0);
    report("phase=present hr=0x%08lx\n", static_cast<unsigned long>(hr));
    report("phase=complete success=%s\n", SUCCEEDED(hr) ? "true" : "false");

    CloseHandle(event);
    fence->Release();
    list->Release();
    allocator->Release();
    backbuffer->Release();
    swapchain->Release();
    if (rtv_heap) rtv_heap->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    if (g_report) fclose(g_report);
    return SUCCEEDED(hr) ? 0 : 15;
}
