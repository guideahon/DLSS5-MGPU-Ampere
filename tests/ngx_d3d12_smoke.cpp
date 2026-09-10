#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>
#include <initializer_list>
#include <stdint.h>
#include <stddef.h>

#include "nvsdk_ngx.h"

struct Vkd3dInteropDevice;
struct Vkd3dInteropDevice5Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Vkd3dInteropDevice*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(Vkd3dInteropDevice*);
    ULONG (STDMETHODCALLTYPE *Release)(Vkd3dInteropDevice*);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(Vkd3dInteropDevice*, REFIID, void**);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(Vkd3dInteropDevice*, UINT*, const char**);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(Vkd3dInteropDevice*, UINT*, const char**);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(Vkd3dInteropDevice*, const void**);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(Vkd3dInteropDevice*, void**, void**, void**);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(Vkd3dInteropDevice*, ID3D12CommandQueue*, void**, UINT32*);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(Vkd3dInteropDevice*, ID3D12Resource*, D3D12_RESOURCE_STATES, int*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(Vkd3dInteropDevice*, ID3D12Resource*, UINT64*, UINT64*);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(Vkd3dInteropDevice*, ID3D12Resource*, UINT64*, UINT64*, int*);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(Vkd3dInteropDevice*, const D3D12_COMMAND_QUEUE_DESC*, UINT32, ID3D12CommandQueue**);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(Vkd3dInteropDevice*, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator**);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(Vkd3dInteropDevice*, ID3D12CommandList*, void**);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(Vkd3dInteropDevice*, ID3D12CommandList*);
    HRESULT (STDMETHODCALLTYPE *LockVulkanQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *UnlockVulkanQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHeapInfo)(Vkd3dInteropDevice*, ID3D12Heap*, UINT64*, UINT64*, UINT32*);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanHeapFd)(Vkd3dInteropDevice*, ID3D12Heap*, UINT32, INT*);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanFenceFd)(Vkd3dInteropDevice*, ID3D12Fence*, UINT32, INT*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanPhysicalDeviceIdentity)(Vkd3dInteropDevice*, UINT8*, UINT32*, UINT32*, UINT32*, UINT32*);
};
struct Vkd3dInteropDevice5 { const Vkd3dInteropDevice5Vtbl* lpVtbl; };
static const GUID kVkd3dInteropDevice5 =
    {0x5f7f64b7, 0x8e0d, 0x4aa8, {0x9e, 0x29, 0x4b, 0x2f, 0x1b, 0x3d, 0x7e, 0x61}};

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

    auto report_physical_identity = [&](ID3D12Device* target, const char* label) {
        Vkd3dInteropDevice5* interop = nullptr;
        HRESULT query = target->QueryInterface(kVkd3dInteropDevice5,
                                                reinterpret_cast<void**>(&interop));
        if (FAILED(query) || interop == nullptr) {
            report("%s physical_identity query=0x%08lx unavailable\n",
                   label, (unsigned long)query);
            return;
        }
        UINT8 uuid[16]{};
        UINT32 domain = 0, bus = 0, device_id = 0, function = 0;
        HRESULT identity = interop->lpVtbl->GetVulkanPhysicalDeviceIdentity(
            reinterpret_cast<Vkd3dInteropDevice*>(interop), uuid, &domain, &bus,
            &device_id, &function);
        report("%s physical_identity hr=0x%08lx uuid=%02x:%02x:%02x:%02x pci=%u:%u:%u.%u\n",
               label, (unsigned long)identity, uuid[0], uuid[1], uuid[2], uuid[3],
               domain, bus, device_id, function);
        interop->lpVtbl->Release(reinterpret_cast<Vkd3dInteropDevice*>(interop));
    };
    report_physical_identity(device, "main_device");

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
            if (SUCCEEDED(device_b_hr))
                report_physical_identity(device_b, "second_device");
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
            ID3D12CommandQueue* command_queue = nullptr;
            D3D12_COMMAND_QUEUE_DESC queue_desc{};
            queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            HRESULT queue_hr = device->CreateCommandQueue(
                &queue_desc, IID_PPV_ARGS(&command_queue));
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
                ID3D12Resource* output_readback = nullptr;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
                UINT output_rows = 0;
                UINT64 output_row_size = 0;
                UINT64 output_readback_size = 0;
                if (output) {
                    D3D12_RESOURCE_DESC output_desc = output->GetDesc();
                    device->GetCopyableFootprints(&output_desc, 0, 1, 0,
                                                  &output_footprint, &output_rows,
                                                  &output_row_size, &output_readback_size);
                }
                if (output && output_readback_size) {
                    D3D12_HEAP_PROPERTIES readback_heap{};
                    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
                    readback_heap.CreationNodeMask = 1;
                    readback_heap.VisibleNodeMask = 1;
                    D3D12_RESOURCE_DESC readback_desc{};
                    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    readback_desc.Width = output_readback_size;
                    readback_desc.Height = 1;
                    readback_desc.DepthOrArraySize = 1;
                    readback_desc.MipLevels = 1;
                    readback_desc.SampleDesc.Count = 1;
                    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    device->CreateCommittedResource(
                        &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                        IID_PPV_ARGS(&output_readback));
                }
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
                    if (output_readback) {
                        D3D12_RESOURCE_BARRIER output_to_copy{};
                        output_to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        output_to_copy.Transition.pResource = output;
                        output_to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        output_to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        output_to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        command_list->ResourceBarrier(1, &output_to_copy);
                        D3D12_TEXTURE_COPY_LOCATION readback_location{};
                        readback_location.pResource = output_readback;
                        readback_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        readback_location.PlacedFootprint = output_footprint;
                        D3D12_TEXTURE_COPY_LOCATION output_location{};
                        output_location.pResource = output;
                        output_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        output_location.SubresourceIndex = 0;
                        command_list->CopyTextureRegion(&readback_location, 0, 0, 0,
                                                        &output_location, nullptr);
                        output_to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        output_to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        command_list->ResourceBarrier(1, &output_to_copy);
                    }
                    HRESULT close_hr = command_list->Close();
                    HRESULT execute_hr = close_hr;
                    HRESULT wait_hr = S_OK;
                    if (SUCCEEDED(close_hr) && SUCCEEDED(queue_hr)) {
                        ID3D12CommandList* command_lists[] = {command_list};
                        command_queue->ExecuteCommandLists(1, command_lists);
                        ID3D12Fence* fence = nullptr;
                        execute_hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                         IID_PPV_ARGS(&fence));
                        if (SUCCEEDED(execute_hr)) {
                            execute_hr = command_queue->Signal(fence, 1);
                            HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                            if (!event) {
                                wait_hr = E_FAIL;
                            } else {
                                if (SUCCEEDED(execute_hr) && fence->GetCompletedValue() < 1)
                                    wait_hr = fence->SetEventOnCompletion(1, event);
                                if (SUCCEEDED(wait_hr) && fence->GetCompletedValue() < 1) {
                                    DWORD wait_result = WaitForSingleObject(event, 10000);
                                    if (wait_result == WAIT_TIMEOUT)
                                        wait_hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                                    else if (wait_result == WAIT_FAILED)
                                        wait_hr = HRESULT_FROM_WIN32(GetLastError());
                                }
                                CloseHandle(event);
                            }
                            fence->Release();
                        }
                    } else if (SUCCEEDED(close_hr)) {
                        execute_hr = queue_hr;
                    }
                    report("D3D12 command submission: queue=0x%08lx close=0x%08lx execute=0x%08lx wait=0x%08lx\n",
                           (unsigned long)queue_hr, (unsigned long)close_hr,
                           (unsigned long)execute_hr, (unsigned long)wait_hr);
                    if (output_readback && SUCCEEDED(execute_hr) && SUCCEEDED(wait_hr)) {
                        void* mapped = nullptr;
                        D3D12_RANGE read_range{0, static_cast<SIZE_T>(output_readback_size)};
                        HRESULT map_hr = output_readback->Map(0, &read_range, &mapped);
                        uint64_t hash = 1469598103934665603ULL;
                        UINT64 nonzero = 0;
                        if (SUCCEEDED(map_hr) && mapped != nullptr) {
                            const unsigned char* bytes = static_cast<const unsigned char*>(mapped);
                            for (UINT64 i = 0; i < output_readback_size; ++i) {
                                if (bytes[i] != 0) ++nonzero;
                                hash ^= bytes[i];
                                hash *= 1099511628211ULL;
                            }
                            D3D12_RANGE written{0, 0};
                            output_readback->Unmap(0, &written);
                        }
                        report("D3D12 output readback: map=0x%08lx bytes=%llu nonzero=%llu fnv1a=0x%016llx\n",
                               (unsigned long)map_hr,
                               (unsigned long long)output_readback_size,
                               (unsigned long long)nonzero,
                               (unsigned long long)hash);
                    }
                } else {
                    report("resource creation failed color=%p output=%p motion=%p depth=%p\n",
                           color, output, motion, depth);
                }
                if (depth) depth->Release();
                if (motion) motion->Release();
                if (output) output->Release();
                if (output_readback) output_readback->Release();
                if (color) color->Release();
            }
            if (handle) {
                NVSDK_NGX_Result release_result = release_feature(handle);
                report("NVSDK_NGX_D3D12_ReleaseFeature: 0x%08x\n", (unsigned int)release_result);
            }
            if (command_list) {
                command_list->Release();
            }
            if (command_queue) command_queue->Release();
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
