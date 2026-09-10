#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/d3d12/d3d12.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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

static UINT env_uint(const wchar_t *name, UINT fallback) {
    wchar_t value[32]{};
    DWORD length = GetEnvironmentVariableW(name, value, sizeof(value) / sizeof(value[0]));
    if (length == 0 || length >= sizeof(value) / sizeof(value[0]))
        return fallback;
    return static_cast<UINT>(wcstoul(value, nullptr, 10));
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

class MessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char *message_text,
                 const char *file, int line) override {
        report("nvrhi_message severity=%u text=%s file=%s line=%d\n",
               static_cast<unsigned int>(severity), message_text ? message_text : "",
               file ? file : "", line);
    }
};

int main() {
    g_report = fopen("nvrhi_d3d12_smoke.result.txt", "a");
    const UINT adapter_index = env_uint(L"MGPU_D3D12_ADAPTER_INDEX", 0);
    report("phase=begin adapter_index=%u\n", adapter_index);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"DLSS5MGPUNvrhiSmoke";
    if (!RegisterClassExW(&window_class)) {
        report("phase=register_window failed error=%lu\n",
               static_cast<unsigned long>(GetLastError()));
        return 2;
    }
    HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"NVRHI smoke",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                640, 360, nullptr, nullptr, window_class.hInstance,
                                nullptr);
    report("phase=create_window hwnd=%p\n", hwnd);
    if (!hwnd) return 3;

    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    report("phase=create_factory hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 4;

    IDXGIAdapter1 *adapter = nullptr;
    hr = factory->EnumAdapters1(adapter_index, &adapter);
    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (SUCCEEDED(hr) && adapter)
        adapter->GetDesc1(&adapter_desc);
    report("phase=select_adapter hr=0x%08lx name=%ls\n",
           static_cast<unsigned long>(hr), adapter ? adapter_desc.Description : L"");
    if (FAILED(hr) || !adapter) return 5;

    ID3D12Device *device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                           IID_PPV_ARGS(&device));
    report("phase=create_device hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 6;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    report("phase=create_queue hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 7;

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = 640;
    swap_desc.Height = 360;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 3;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *swapchain = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &swap_desc, nullptr,
                                         nullptr, &swapchain);
    report("phase=create_swapchain hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 8;

    MessageCallback callback;
    report("phase=nvrhi_device_begin\n");
    nvrhi::d3d12::Device *nvrhi_device =
        new nvrhi::d3d12::Device(&callback, device, queue);
    report("phase=nvrhi_device_complete ptr=%p\n", nvrhi_device);
    if (!nvrhi_device) return 9;

    nvrhi::TextureDesc texture_desc;
    texture_desc.width = 640;
    texture_desc.height = 360;
    texture_desc.format = nvrhi::Format::RGBA8_UNORM;
    texture_desc.debugName = "SwapChainBuffer";
    texture_desc.isRenderTarget = true;
    texture_desc.initialState = nvrhi::ResourceStates::PRESENT;
    texture_desc.keepInitialState = true;

    bool all_wrapped = true;
    nvrhi::TextureHandle first_texture;
    for (UINT index = 0; index < swap_desc.BufferCount; ++index) {
        ID3D12Resource *resource = nullptr;
        HRESULT buffer_hr = swapchain->GetBuffer(index, IID_PPV_ARGS(&resource));
        report("phase=get_backbuffer index=%u hr=0x%08lx ptr=%p\n", index,
               static_cast<unsigned long>(buffer_hr), resource);
        if (FAILED(buffer_hr) || !resource) {
            all_wrapped = false;
            continue;
        }
        nvrhi::TextureHandle texture = nvrhi_device->createHandleForNativeTexture(
            nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(resource), texture_desc);
        report("phase=wrap_backbuffer index=%u handle=%s\n", index,
               texture ? "set" : "null");
        if (!texture)
            all_wrapped = false;
        if (index == 0)
            first_texture = texture;
        resource->Release();
    }

    if (first_texture) {
        nvrhi::FramebufferDesc framebuffer_desc;
        framebuffer_desc.addColorAttachment(first_texture.Get());
        nvrhi::FramebufferHandle framebuffer = nvrhi_device->createFramebuffer(framebuffer_desc);
        report("phase=create_backbuffer_framebuffer handle=%s\n",
               framebuffer ? "set" : "null");
        if (!framebuffer)
            all_wrapped = false;
    } else {
        all_wrapped = false;
    }

    nvrhi::TextureDesc shadow_desc;
    shadow_desc.width = 2048;
    shadow_desc.height = 2048;
    shadow_desc.sampleCount = 1;
    shadow_desc.isRenderTarget = true;
    shadow_desc.isTypeless = true;
    shadow_desc.format = nvrhi::Format::D24S8;
    shadow_desc.debugName = "ShadowMap";
    shadow_desc.useClearValue = true;
    shadow_desc.clearValue = nvrhi::Color(1.f);
    shadow_desc.initialState = nvrhi::ResourceStates::SHADER_RESOURCE;
    shadow_desc.keepInitialState = true;
    shadow_desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    shadow_desc.arraySize = 4;
    nvrhi::TextureHandle shadow_texture = nvrhi_device->createTexture(shadow_desc);
    report("phase=create_shadow_map handle=%s\n", shadow_texture ? "set" : "null");
    if (!shadow_texture)
        all_wrapped = false;

    if (GetEnvironmentVariableW(L"MGPU_NVRHI_SHOW_AFTER", nullptr, 0) > 0) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        report("phase=show_window_after_resources success=true\n");
    }

    report("phase=complete success=%s\n", all_wrapped ? "true" : "false");
    nvrhi_device->Release();
    swapchain->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    if (g_report) fclose(g_report);
    return all_wrapped ? 0 : 10;
}
