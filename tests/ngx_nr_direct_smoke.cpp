#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>

#include "nvsdk_ngx.h"

static void set_resource(NVSDK_NGX_Parameter* p, const char* name,
                         ID3D12Resource* resource) {
    using SetResourceFn = void (*)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    auto** table = *reinterpret_cast<void***>(p);
    reinterpret_cast<SetResourceFn>(table[0])(p, name, resource);
}

static ID3D12Resource* make_texture(ID3D12Device* device, UINT width, UINT height,
                                    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                    D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               state, nullptr, IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

int main() {
    FILE* report_file = fopen("ngx_nr_direct_smoke.result.txt", "a");
    auto report = [&](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        if (report_file) {
            va_list copy;
            va_copy(copy, args);
            vfprintf(report_file, fmt, copy);
            va_end(copy);
            fflush(report_file);
        }
        vfprintf(stderr, fmt, args);
        va_end(args);
        fflush(stderr);
    };

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    report("CreateDXGIFactory1: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 2;

    IDXGIAdapter1* adapter = nullptr;
    UINT index = 0;
    while (factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        report("Adapter[%u]: %ls flags=0x%lx\n", index, desc.Description,
               (unsigned long)desc.Flags);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc.Description, L"RTX 3090") != nullptr)
            break;
        adapter->Release();
        adapter = nullptr;
        ++index;
    }
    if (!adapter) {
        report("No RTX 3090 DXGI adapter\n");
        factory->Release();
        return 3;
    }

    ID3D12Device* device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    report("D3D12CreateDevice: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 4;

    HMODULE proxy = LoadLibraryW(L"nvngx_dlss.dll");
    HMODULE nr = LoadLibraryW(L"nvngx_dlssnr.dll");
    report("LoadLibrary proxy/nr: %s/%s error=%lu\n", proxy ? "ok" : "failed",
           nr ? "ok" : "failed", GetLastError());
    if (!proxy || !nr) return 5;

    using InitFn = NVSDK_NGX_Result (WINAPI *)(unsigned long long, const wchar_t*,
                                                ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_Parameter*);
    using ShutdownFn = NVSDK_NGX_Result (WINAPI *)(ID3D12Device*);
    using AllocFn = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Parameter**);
    using DestroyFn = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Parameter*);
    using CreateFn = NVSDK_NGX_Result (WINAPI *)(ID3D12GraphicsCommandList*,
                                                  NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter*,
                                                  NVSDK_NGX_Handle**);
    using EvalFn = NVSDK_NGX_Result (WINAPI *)(ID3D12GraphicsCommandList*,
                                                const NVSDK_NGX_Handle*,
                                                const NVSDK_NGX_Parameter*,
                                                PFN_NVSDK_NGX_ProgressCallback);
    using ReleaseFn = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Handle*);

    auto init = reinterpret_cast<InitFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_Init_Ext"));
    auto nr_init = reinterpret_cast<InitFn>(GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext"));
    auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_Shutdown1"));
    auto allocate = reinterpret_cast<AllocFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_DestroyParameters"));
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_CreateFeature"));
    auto evaluate = reinterpret_cast<EvalFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto release = reinterpret_cast<ReleaseFn>(GetProcAddress(proxy, "NVSDK_NGX_D3D12_ReleaseFeature"));
    report("exports proxy_init=%s nr_init=%s shutdown=%s alloc=%s destroy=%s create=%s eval=%s release=%s\n",
           init ? "ok" : "missing", nr_init ? "ok" : "missing",
           shutdown ? "ok" : "missing",
           allocate ? "ok" : "missing", destroy ? "ok" : "missing",
           create ? "ok" : "missing", evaluate ? "ok" : "missing",
           release ? "ok" : "missing");
    if (!init || !shutdown || !allocate || !destroy || !create || !evaluate || !release)
        return 6;

    const unsigned long long app_id = 231313132ULL;
    NVSDK_NGX_Result nr_init_result = nr_init
        ? nr_init(app_id, L".", device, NVSDK_NGX_Version_API, nullptr)
        : NVSDK_NGX_Result_Fail;
    report("direct NR NVSDK_NGX_D3D12_Init_Ext: 0x%08x\n", (unsigned int)nr_init_result);
    NVSDK_NGX_Result init_result = init(app_id, L".", device, NVSDK_NGX_Version_API, nullptr);
    report("proxy NVSDK_NGX_D3D12_Init_Ext: 0x%08x\n", (unsigned int)init_result);
    if (!NVSDK_NGX_SUCCEED(init_result)) return 7;

    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Result alloc_result = allocate(&params);
    report("AllocateParameters: 0x%08x ptr=%s\n", (unsigned int)alloc_result,
           params ? "set" : "null");
    if (!params || !NVSDK_NGX_SUCCEED(alloc_result)) return 8;

    constexpr UINT in_w = 640, in_h = 360, out_w = 1280, out_h = 720;
    params->Set("DLSSNR.Width", in_w);
    params->Set("DLSSNR.Height", in_h);
    params->Set("DLSSNR.Hint.Render.Preset", 1);
    params->Set(NVSDK_NGX_Parameter_Width, in_w);
    params->Set(NVSDK_NGX_Parameter_Height, in_h);
    params->Set(NVSDK_NGX_Parameter_OutWidth, out_w);
    params->Set(NVSDK_NGX_Parameter_OutHeight, out_h);
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                (int)NVSDK_NGX_PerfQuality_Value_MaxQuality);
    params->Set("DLSSNR.ScalingRatio", 2.0f);
    params->Set("DLSSNR.MVecScaleX", 1.0f);
    params->Set("DLSSNR.MVecScaleY", 1.0f);
    params->Set("DLSSNR.Reset", 1);
    params->Set("DLSSNR.Enabled", 1);

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT allocator_hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator));
    HRESULT list_hr = allocator_hr == S_OK
        ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                     IID_PPV_ARGS(&list))
        : allocator_hr;
    report("CreateCommandAllocator/List: 0x%08lx/0x%08lx\n",
           (unsigned long)allocator_hr, (unsigned long)list_hr);

    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result create_result = NVSDK_NGX_Result_Fail;
    if (list_hr == S_OK) {
        create_result = create(list, NVSDK_NGX_Feature_Reserved18, params, &handle);
    }
    report("CreateFeature Reserved18: 0x%08x handle=%s\n", (unsigned int)create_result,
           handle ? "set" : "null");

    ID3D12Resource* color = make_texture(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* output = make_texture(device, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* motion = make_texture(device, in_w, in_h, DXGI_FORMAT_R16G16_FLOAT,
                                           D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = make_texture(device, in_w, in_h, DXGI_FORMAT_R32_FLOAT,
                                          D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    report("Resources color=%p output=%p motion=%p depth=%p\n", color, output, motion, depth);

    NVSDK_NGX_Result eval_result = NVSDK_NGX_Result_Fail;
    if (handle && list_hr == S_OK && color && output && motion && depth) {
        set_resource(params, "DLSSNR.Color", color);
        set_resource(params, "DLSSNR.Output", output);
        set_resource(params, "DLSSNR.MVec", motion);
        set_resource(params, "DLSSNR.Depth", depth);
        params->Set("DLSSNR.ColorSubrectBaseX", 0U);
        params->Set("DLSSNR.ColorSubrectBaseY", 0U);
        params->Set("DLSSNR.ColorSubrectWidth", in_w);
        params->Set("DLSSNR.ColorSubrectHeight", in_h);
        params->Set("DLSSNR.MVecSubrectBaseX", 0U);
        params->Set("DLSSNR.MVecSubrectBaseY", 0U);
        params->Set("DLSSNR.MVecSubrectWidth", in_w);
        params->Set("DLSSNR.MVecSubrectHeight", in_h);
        params->Set("DLSSNR.OutputSubrectBaseX", 0U);
        params->Set("DLSSNR.OutputSubrectBaseY", 0U);
        params->Set("DLSSNR.OutputSubrectWidth", out_w);
        params->Set("DLSSNR.OutputSubrectHeight", out_h);
        eval_result = evaluate(list, handle, params, nullptr);
    }
    report("EvaluateFeature Reserved18: 0x%08x\n", (unsigned int)eval_result);

    if (handle) report("ReleaseFeature: 0x%08x\n", (unsigned int)release(handle));
    if (destroy) destroy(params);
    report("Shutdown1: 0x%08x\n", (unsigned int)shutdown(device));
    if (depth) depth->Release();
    if (motion) motion->Release();
    if (output) output->Release();
    if (color) color->Release();
    if (list) { list->Close(); list->Release(); }
    if (allocator) allocator->Release();
    FreeLibrary(nr);
    device->Release();
    adapter->Release();
    factory->Release();
    if (report_file) fclose(report_file);
    return handle && NVSDK_NGX_SUCCEED(eval_result) ? 0 : 9;
}
