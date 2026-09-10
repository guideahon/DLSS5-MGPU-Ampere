#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>
#include <initializer_list>
#include <stdint.h>

#include "nvsdk_ngx.h"

int main() {
    const unsigned long long app_id = 231313132ULL;
    FILE* report_file = fopen("ngx_d3d12_smoke.result.txt", "a");
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
    UINT adapter_index = 0;
    while (factory->EnumAdapters1(adapter_index, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        report("Adapter[%u]: %ls flags=0x%lx\n", adapter_index, desc.Description, (unsigned long)desc.Flags);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && wcsstr(desc.Description, L"RTX 3090") != nullptr) break;
        adapter->Release();
        adapter = nullptr;
        ++adapter_index;
    }
    if (adapter == nullptr) {
        report("No hardware DXGI adapter\n");
        factory->Release();
        return 3;
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    report("Adapter: %ls\n", desc.Description);

    ID3D12Device* device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    report("D3D12CreateDevice: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        adapter->Release();
        factory->Release();
        return 4;
    }

    ID3D12Device* device_b = nullptr;
    IDXGIAdapter1* adapter_b = nullptr;
    UINT adapter_b_index = adapter_index + 1;
    while (factory->EnumAdapters1(adapter_b_index, &adapter_b) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc_b{};
        adapter_b->GetDesc1(&desc_b);
        if (!(desc_b.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc_b.Description, L"RTX 3090") != nullptr) {
            HRESULT device_b_hr = D3D12CreateDevice(adapter_b, D3D_FEATURE_LEVEL_12_0,
                                                    IID_PPV_ARGS(&device_b));
            report("Second adapter[%u]: %ls D3D12CreateDevice=0x%08lx\n",
                   adapter_b_index, desc_b.Description, (unsigned long)device_b_hr);
            adapter_b->Release();
            adapter_b = nullptr;
            break;
        }
        adapter_b->Release();
        adapter_b = nullptr;
        ++adapter_b_index;
    }

    HMODULE ngx = LoadLibraryW(L"nvngx_dlss.dll");
    report("LoadLibrary nvngx_dlss.dll: %s error=%lu\n", ngx ? "ok" : "failed", GetLastError());
    if (!ngx) return 5;

    typedef NVSDK_NGX_Result (WINAPI *InitFn)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    typedef NVSDK_NGX_Result (WINAPI *ShutdownFn)(ID3D12Device*);
    typedef NVSDK_NGX_Result (WINAPI *GetReqFn)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
    typedef NVSDK_NGX_Result (WINAPI *GetCapsFn)(NVSDK_NGX_Parameter**);
    typedef NVSDK_NGX_Result (WINAPI *DestroyParamsFn)(NVSDK_NGX_Parameter*);
    typedef NVSDK_NGX_Result (WINAPI *AllocateParamsFn)(NVSDK_NGX_Parameter**);
    typedef NVSDK_NGX_Result (WINAPI *ScratchFn)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
    typedef NVSDK_NGX_Result (WINAPI *CreateFn)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    typedef NVSDK_NGX_Result (WINAPI *EvaluateFn)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
    typedef NVSDK_NGX_Result (WINAPI *ReleaseFn)(NVSDK_NGX_Handle*);
    InitFn init = (InitFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext");
    ShutdownFn shutdown = (ShutdownFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown1");
    GetReqFn get_req = (GetReqFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    GetCapsFn get_caps = (GetCapsFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    DestroyParamsFn destroy_params = (DestroyParamsFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters");
    AllocateParamsFn allocate_params = (AllocateParamsFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
    ScratchFn get_scratch = (ScratchFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetScratchBufferSize");
    CreateFn create_feature = (CreateFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
    EvaluateFn evaluate_feature = (EvaluateFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
    ReleaseFn release_feature = (ReleaseFn)GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");
    report("exports init=%s shutdown=%s req=%s caps=%s destroy=%s\n",
           init ? "ok" : "missing", shutdown ? "ok" : "missing",
           get_req ? "ok" : "missing", get_caps ? "ok" : "missing",
           destroy_params ? "ok" : "missing");
    if (!init || !shutdown) return 6;

    if (get_req) {
        NVSDK_NGX_FeatureDiscoveryInfo discovery{};
        discovery.SDKVersion = NVSDK_NGX_Version_API;
        discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
        discovery.Identifier.v.ApplicationId = 0x0023ULL;
        discovery.ApplicationDataPath = L".";
        for (NVSDK_NGX_Feature feature : {NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_FrameGeneration}) {
            discovery.FeatureID = feature;
            NVSDK_NGX_FeatureRequirement requirement{};
            NVSDK_NGX_Result req_result = get_req(adapter, &discovery, &requirement);
            report("GetFeatureRequirements feature=%u result=0x%08x support=%u min_arch=0x%x os=%s\n",
                   (unsigned int)feature, (unsigned int)req_result,
                   (unsigned int)requirement.FeatureSupported,
                   requirement.MinHWArchitecture, requirement.MinOSVersion);
        }
    }

    bool init_second_first = device_b &&
        GetEnvironmentVariableW(L"MGPU_NGX_SECOND_DEVICE_FIRST", nullptr, 0) > 0;
    NVSDK_NGX_Result early_result_b = NVSDK_NGX_Result_Fail;
    if (init_second_first) {
        early_result_b = init(app_id, L".", device_b, NVSDK_NGX_Version_API, nullptr);
        report("Early second device NVSDK_NGX_D3D12_Init_Ext: 0x%08x\n",
               (unsigned int)early_result_b);
    }
    NVSDK_NGX_Result result = init(app_id, L".", device, NVSDK_NGX_Version_API, nullptr);
    report("NVSDK_NGX_D3D12_Init_Ext: 0x%08x\n", (unsigned int)result);

    // Optional two-device gate. It is disabled by default so the baseline
    // smoke remains identical. When enabled, both adapters are initialized
    // in one process and GPU B attempts a small feature creation as well.
    bool second_device_initialized = false;
    NVSDK_NGX_Parameter* params_b = nullptr;
    NVSDK_NGX_Handle* handle_b = nullptr;
    ID3D12CommandAllocator* allocator_b = nullptr;
    ID3D12GraphicsCommandList* list_b = nullptr;
    if (device_b && GetEnvironmentVariableW(L"MGPU_NGX_SECOND_DEVICE_TEST", nullptr, 0) > 0 &&
        NVSDK_NGX_SUCCEED(result) && allocate_params && create_feature && release_feature) {
        NVSDK_NGX_Result init_b = init_second_first
            ? early_result_b
            : init(app_id, L".", device_b, NVSDK_NGX_Version_API, nullptr);
        if (!init_second_first)
            report("Second device NVSDK_NGX_D3D12_Init_Ext: 0x%08x\n", (unsigned int)init_b);
        second_device_initialized = NVSDK_NGX_SUCCEED(init_b);
        NVSDK_NGX_Result alloc_b = allocate_params(&params_b);
        report("Second device AllocateParameters: 0x%08x ptr=%s\n",
               (unsigned int)alloc_b, params_b ? "set" : "null");
        if (NVSDK_NGX_SUCCEED(init_b) && params_b && NVSDK_NGX_SUCCEED(alloc_b)) {
            params_b->Set(NVSDK_NGX_Parameter_Width, 640U);
            params_b->Set(NVSDK_NGX_Parameter_Height, 360U);
            params_b->Set(NVSDK_NGX_Parameter_OutWidth, 1280U);
            params_b->Set(NVSDK_NGX_Parameter_OutHeight, 720U);
            params_b->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                          (int)NVSDK_NGX_PerfQuality_Value_MaxQuality);
            params_b->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1U);
            params_b->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1U);
            HRESULT allocator_b_hr = device_b->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_b));
            HRESULT list_b_hr = allocator_b_hr == S_OK
                ? device_b->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              allocator_b, nullptr, IID_PPV_ARGS(&list_b))
                : allocator_b_hr;
            NVSDK_NGX_Result create_b = NVSDK_NGX_Result_Fail;
            if (list_b_hr == S_OK)
                create_b = create_feature(list_b, NVSDK_NGX_Feature_SuperSampling,
                                          params_b, &handle_b);
            report("Second device CreateFeature: 0x%08x handle=%s allocator=0x%08lx list=0x%08lx\n",
                   (unsigned int)create_b, handle_b ? "set" : "null",
                   (unsigned long)allocator_b_hr, (unsigned long)list_b_hr);
        }
    }
    if (NVSDK_NGX_SUCCEED(result) && get_caps) {
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Result caps_result = get_caps(&parameters);
        report("NVSDK_NGX_D3D12_GetCapabilityParameters: 0x%08x ptr=%s\n",
               (unsigned int)caps_result, parameters ? "set" : "null");
        if (parameters && destroy_params) destroy_params(parameters);
    }
    if (NVSDK_NGX_SUCCEED(result) && allocate_params && get_scratch && create_feature && evaluate_feature && release_feature) {
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Result alloc_result = allocate_params(&parameters);
        report("NVSDK_NGX_D3D12_AllocateParameters: 0x%08x ptr=%s\n",
               (unsigned int)alloc_result, parameters ? "set" : "null");
        if (parameters && NVSDK_NGX_SUCCEED(alloc_result)) {
            parameters->Set(NVSDK_NGX_Parameter_Width, 640U);
            parameters->Set(NVSDK_NGX_Parameter_Height, 360U);
            parameters->Set(NVSDK_NGX_Parameter_OutWidth, 1280U);
            parameters->Set(NVSDK_NGX_Parameter_OutHeight, 720U);
            parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int)NVSDK_NGX_PerfQuality_Value_MaxQuality);
            size_t scratch_size = 0;
            NVSDK_NGX_Result scratch_result = get_scratch(NVSDK_NGX_Feature_SuperSampling, parameters, &scratch_size);
            report("NVSDK_NGX_D3D12_GetScratchBufferSize: 0x%08x bytes=%llu\n",
                   (unsigned int)scratch_result, (unsigned long long)scratch_size);

            ID3D12CommandAllocator* allocator = nullptr;
            ID3D12GraphicsCommandList* command_list = nullptr;
            HRESULT allocator_hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            HRESULT list_hr = allocator_hr == S_OK
                ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&command_list))
                : allocator_hr;
            NVSDK_NGX_Handle* handle = nullptr;
            NVSDK_NGX_Result create_result = NVSDK_NGX_Result_Fail;
            if (list_hr == S_OK) {
                NVSDK_NGX_DLSS_Create_Params create_params{};
                create_params.Feature.InWidth = 640;
                create_params.Feature.InHeight = 360;
                create_params.Feature.InTargetWidth = 1280;
                create_params.Feature.InTargetHeight = 720;
                create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
                create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
                create_params.InEnableOutputSubrects = false;
                parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1U);
                parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1U);
                parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int)create_params.InFeatureCreateFlags);
                create_result = create_feature(command_list, NVSDK_NGX_Feature_SuperSampling, parameters, &handle);
            }
            report("NVSDK_NGX_D3D12_CreateFeature: 0x%08x handle=%s allocator=0x%08lx list=0x%08lx\n",
                   (unsigned int)create_result, handle ? "set" : "null",
                   (unsigned long)allocator_hr, (unsigned long)list_hr);
            if (handle && list_hr == S_OK) {
                auto make_texture = [&](UINT width, UINT height, DXGI_FORMAT format,
                                        D3D12_RESOURCE_FLAGS flags,
                                        D3D12_RESOURCE_STATES state) -> ID3D12Resource* {
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
                    HRESULT create_hr = device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                        IID_PPV_ARGS(&resource));
                    if (create_hr != S_OK) return nullptr;
                    return resource;
                };
                ID3D12Resource* color = make_texture(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                      D3D12_RESOURCE_FLAG_NONE,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ID3D12Resource* output = make_texture(1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12Resource* motion = make_texture(640, 360, DXGI_FORMAT_R16G16_FLOAT,
                                                       D3D12_RESOURCE_FLAG_NONE,
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ID3D12Resource* depth = make_texture(640, 360, DXGI_FORMAT_R32_FLOAT,
                                                      D3D12_RESOURCE_FLAG_NONE,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (color && output && motion && depth) {
                    // The Windows NGX parameter ABI used by the runtime places
                    // D3D12 resources in its compact resource slot. Use that
                    // slot explicitly so the proxy can recover game resources.
                    auto set_resource = [](NVSDK_NGX_Parameter* target,
                                           const char* name, ID3D12Resource* resource) {
                        using SetResourceFn = void (*)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
                        auto** table = *reinterpret_cast<void***>(target);
                        reinterpret_cast<SetResourceFn>(table[0])(target, name, resource);
                    };
                    set_resource(parameters, NVSDK_NGX_Parameter_Color, color);
                    set_resource(parameters, NVSDK_NGX_Parameter_Output, output);
                    set_resource(parameters, NVSDK_NGX_Parameter_MotionVectors, motion);
                    set_resource(parameters, NVSDK_NGX_Parameter_Depth, depth);
                    parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
                    parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
                    parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
                    parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
                    parameters->Set(NVSDK_NGX_Parameter_Reset, 1);
                    // Match the public NGX D3D12 helper contract, including
                    // zero-based subrects and render dimensions. The earlier
                    // probe only supplied resource handles and dimensions,
                    // which made it impossible to distinguish a bad host
                    // contract from a runtime/bridge rejection.
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 640U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 360U);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
                    parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
                    set_resource(parameters, "DLSSNR.Color", color);
                    set_resource(parameters, "DLSSNR.Output", output);
                    set_resource(parameters, "DLSSNR.MVec", motion);
                    set_resource(parameters, "DLSSNR.Depth", depth);
                    parameters->Set("DLSSNR.ColorSubrectWidth", 640U);
                    parameters->Set("DLSSNR.ColorSubrectHeight", 360U);
                    parameters->Set("DLSSNR.OutputSubrectWidth", 1280U);
                    parameters->Set("DLSSNR.OutputSubrectHeight", 720U);
                    parameters->Set("DLSSNR.MVecSubrectWidth", 640U);
                    parameters->Set("DLSSNR.MVecSubrectHeight", 360U);
                    parameters->Set("DLSSNR.DepthSubrectWidth", 640U);
                    parameters->Set("DLSSNR.DepthSubrectHeight", 360U);
                    parameters->Set("DLSSNR.MVecScaleX", 1.0f);
                    parameters->Set("DLSSNR.MVecScaleY", 1.0f);
                    parameters->Set("DLSSNR.Reset", 1);
                    parameters->Set("DLSSNR.Enabled", 1);
                    command_list->Close();
                    allocator->Reset();
                    command_list->Reset(allocator, nullptr);
                    NVSDK_NGX_Result evaluate_result = evaluate_feature(command_list, handle, parameters, nullptr);
                    report("NVSDK_NGX_D3D12_EvaluateFeature: 0x%08x resources=color/output/motion/depth\n",
                           (unsigned int)evaluate_result);
                } else {
                    report("resource creation failed color=%p output=%p motion=%p depth=%p\n",
                           color, output, motion, depth);
                }
                if (depth) depth->Release();
                if (motion) motion->Release();
                if (output) output->Release();
                if (color) color->Release();
            }
            if (handle) {
                NVSDK_NGX_Result release_result = release_feature(handle);
                report("NVSDK_NGX_D3D12_ReleaseFeature: 0x%08x\n", (unsigned int)release_result);
            }
            if (command_list) {
                command_list->Close();
                command_list->Release();
            }
            if (allocator) allocator->Release();
            if (destroy_params) destroy_params(parameters);
        }
    }
    if (handle_b)
        report("Second device ReleaseFeature: 0x%08x\n",
               (unsigned int)release_feature(handle_b));
    if (destroy_params && params_b) destroy_params(params_b);
    if (list_b) { list_b->Close(); list_b->Release(); }
    if (allocator_b) allocator_b->Release();
    if (second_device_initialized)
        report("Second device Shutdown1: 0x%08x\n",
               (unsigned int)shutdown(device_b));

    NVSDK_NGX_Result shutdown_result = shutdown(device);
    report("NVSDK_NGX_D3D12_Shutdown1: 0x%08x\n", (unsigned int)shutdown_result);

    FreeLibrary(ngx);
    if (device_b) device_b->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    if (report_file) fclose(report_file);
    return NVSDK_NGX_SUCCEED(result) ? 0 : 7;
}
