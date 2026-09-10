#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>

/*
 * This is the small, public-in-practice VKD3D-Proton interop surface that is
 * shipped by GE-Proton.  We keep a local declaration so the probe can be
 * built without generating VKD3D's IDL headers.  It is intentionally limited
 * to the first two methods we need; the rest of the vtable is opaque here.
 */
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;

struct vkd3d_device_ext;
struct vkd3d_device_ext_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(vkd3d_device_ext *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(vkd3d_device_ext *);
    ULONG (STDMETHODCALLTYPE *Release)(vkd3d_device_ext *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(vkd3d_device_ext *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    BOOL (STDMETHODCALLTYPE *GetExtensionSupport)(vkd3d_device_ext *, UINT);
};
struct vkd3d_device_ext {
    const vkd3d_device_ext_vtbl *lpVtbl;
};

struct vkd3d_interop_device;
struct vkd3d_interop_device_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(vkd3d_interop_device *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(vkd3d_interop_device *);
    ULONG (STDMETHODCALLTYPE *Release)(vkd3d_interop_device *);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(vkd3d_interop_device *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(vkd3d_interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(vkd3d_interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(vkd3d_interop_device *, const void **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(vkd3d_interop_device *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(vkd3d_interop_device *, ID3D12CommandQueue *, void **, UINT32 *);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(vkd3d_interop_device *, ID3D12Resource *, D3D12_RESOURCE_STATES, int *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(vkd3d_interop_device *, ID3D12Resource *, UINT64 *, UINT64 *);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(vkd3d_interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(vkd3d_interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(vkd3d_interop_device *, ID3D12Resource *, UINT64 *, UINT64 *, int *);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(vkd3d_interop_device *, const D3D12_COMMAND_QUEUE_DESC *, UINT32, ID3D12CommandQueue **);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(vkd3d_interop_device *, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator **);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(vkd3d_interop_device *, ID3D12CommandList *, void **);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(vkd3d_interop_device *, ID3D12CommandList *);
    HRESULT (STDMETHODCALLTYPE *LockVulkanQueue)(vkd3d_interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockVulkanQueue)(vkd3d_interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHeapInfo)(vkd3d_interop_device *, ID3D12Heap *, UINT64 *, UINT64 *, UINT32 *);
};
struct vkd3d_interop_device {
    const vkd3d_interop_device_vtbl *lpVtbl;
};

struct vk_memory_get_fd_info {
    UINT32 sType;
    const void *pNext;
    void *memory;
    UINT32 handleType;
};
struct vk_memory_fd_properties {
    UINT32 sType;
    const void *pNext;
    UINT32 memoryTypeBits;
};
using vk_get_instance_proc_addr_fn = void *(WINAPI *)(VkInstance, const char *);
using vk_get_device_proc_addr_fn = void *(WINAPI *)(VkDevice, const char *);
using vk_get_memory_fd_fn = int (WINAPI *)(VkDevice, const vk_memory_get_fd_info *, int *);
using vk_get_memory_fd_properties_fn = int (WINAPI *)(VkDevice, UINT32, int, vk_memory_fd_properties *);

/* Minimal CUDA Driver API declarations.  Loading nvcuda.dll dynamically keeps
 * this PE probe independent from the Linux CUDA stub library at link time. */
using CUresult = int;
using CUdevice = int;
using CUcontext = void *;
using CUexternalMemory = void *;
using CUdeviceptr = unsigned long long;
struct cuda_external_memory_handle_desc {
    int type;
    union {
        int fd;
        struct { void *handle; const void *name; } win32;
        const void *nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};
struct cuda_external_memory_buffer_desc {
    unsigned long long offset;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};
using cu_init_fn = CUresult (WINAPI *)(unsigned int);
using cu_device_get_fn = CUresult (WINAPI *)(CUdevice *, int);
using cu_ctx_create_fn = CUresult (WINAPI *)(CUcontext *, unsigned int, CUdevice);
using cu_ctx_destroy_fn = CUresult (WINAPI *)(CUcontext);
using cu_import_external_memory_fn = CUresult (WINAPI *)(CUexternalMemory *, const cuda_external_memory_handle_desc *);
using cu_external_memory_get_mapped_buffer_fn = CUresult (WINAPI *)(CUdeviceptr *, CUexternalMemory, const cuda_external_memory_buffer_desc *);
using cu_mem_free_fn = CUresult (WINAPI *)(CUdeviceptr);
using cu_destroy_external_memory_fn = CUresult (WINAPI *)(CUexternalMemory);
using wine_unix_spawnvp_fn = LONG (WINAPI *)(char * const[], int);

struct cuda_import_result {
    bool initialized = false;
    bool gpu0_imported = false;
    bool gpu1_imported = false;
};

static bool g_cuda_any_imported = false;
static bool g_cuda_helper_spawned = false;

static const GUID IID_ID3D12DeviceExt =
    {0x11ea7a1a, 0x0f6a, 0x49bf, {0xb6, 0x12, 0x3e, 0x30, 0xf8, 0xe2, 0x01, 0xdd}};
static const GUID IID_ID3D12DXVKInteropDevice =
    {0x39da4e09, 0xbd1c, 0x4198, {0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd}};
static const GUID IID_ID3D12DXVKInteropDevice3 =
    {0x22a70184, 0xa6a4, 0x4c24, {0xbf, 0x97, 0x7d, 0x6d, 0xf9, 0xf1, 0x2d, 0x8a}};

struct device_handles {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    bool valid;
};

static void log_hr(const char *label, HRESULT hr)
{
    fprintf(stderr, "%s: 0x%08lx\n", label, (unsigned long)hr);
}

static IDXGIAdapter1 *find_3090(IDXGIFactory4 *factory, UINT *cursor)
{
    IDXGIAdapter1 *adapter = nullptr;
    while (factory->EnumAdapters1(*cursor, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ++*cursor;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc.Description, L"RTX 3090") != nullptr)
        {
            fprintf(stderr, "DXGI adapter index=%u name=%ls luid=%08lx:%08lx\n",
                    (unsigned int)(*cursor - 1), desc.Description,
                    (unsigned long)desc.AdapterLuid.HighPart,
                    (unsigned long)desc.AdapterLuid.LowPart);
            return adapter;
        }
        adapter->Release();
    }
    return nullptr;
}

static device_handles inspect_device(const char *label, ID3D12Device *device)
{
    device_handles result{};
    vkd3d_device_ext *ext = nullptr;
    HRESULT hr = device->QueryInterface(IID_ID3D12DeviceExt, (void **)&ext);
    log_hr(label, hr);
    if (FAILED(hr) || !ext)
        return result;

    hr = ext->lpVtbl->GetVulkanHandles(ext, &result.instance,
            &result.physical, &result.device);
    log_hr("GetVulkanHandles", hr);
    fprintf(stderr, "%s handles: instance=%p physical=%p device=%p\n",
            label, result.instance, result.physical, result.device);

    /* D3D12_VK_NV_OPTICAL_FLOW is enum value 4 in VKD3D's public IDL. */
    BOOL optical_flow = ext->lpVtbl->GetExtensionSupport(ext, 4);
    fprintf(stderr, "%s optical_flow=%s\n", label, optical_flow ? "yes" : "no");

    ext->lpVtbl->Release(ext);
    result.valid = SUCCEEDED(hr) && result.instance && result.physical && result.device;
    return result;
}

static bool inspect_heap_interop(ID3D12Device *device)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = 65536;
    heap_desc.Properties = properties;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;

    ID3D12Heap *heap = nullptr;
    HRESULT hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    log_hr("CreateHeap for VKD3D interop", hr);
    if (FAILED(hr))
        return false;

    vkd3d_interop_device *interop = nullptr;
    hr = device->QueryInterface(IID_ID3D12DXVKInteropDevice3, (void **)&interop);
    log_hr("QueryInterface ID3D12DXVKInteropDevice3", hr);
    if (FAILED(hr) || !interop)
    {
        heap->Release();
        return false;
    }

    UINT64 memory = 0;
    UINT64 offset = 0;
    UINT32 memory_type = 0;
    hr = interop->lpVtbl->GetVulkanHeapInfo(interop, heap, &memory, &offset, &memory_type);
    log_hr("GetVulkanHeapInfo", hr);
    fprintf(stderr, "VKD3D heap: memory=0x%llx offset=%llu memory_type=%u\n",
            (unsigned long long)memory, (unsigned long long)offset,
            (unsigned int)memory_type);

    VkInstance instance = nullptr;
    VkPhysicalDevice physical = nullptr;
    VkDevice vk_device = nullptr;
    vkd3d_device_ext *device_ext = nullptr;
    HRESULT handles_hr = device->QueryInterface(IID_ID3D12DeviceExt,
            (void **)&device_ext);
    if (SUCCEEDED(handles_hr) && device_ext)
    {
        handles_hr = device_ext->lpVtbl->GetVulkanHandles(device_ext,
                &instance, &physical, &vk_device);
        device_ext->lpVtbl->Release(device_ext);
    }

    bool fd_exported = false;
    int exported_fd = -1;
    vk_get_memory_fd_fn get_memory_fd = nullptr;
    vk_get_memory_fd_properties_fn get_memory_fd_properties = nullptr;
    if (SUCCEEDED(handles_hr) && memory && vk_device)
    {
        HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
        auto get_instance_proc_addr = vulkan
            ? reinterpret_cast<vk_get_instance_proc_addr_fn>(
                    GetProcAddress(vulkan, "vkGetInstanceProcAddr"))
            : nullptr;
        auto get_device_proc_addr = get_instance_proc_addr
            ? reinterpret_cast<vk_get_device_proc_addr_fn>(
                    get_instance_proc_addr(instance, "vkGetDeviceProcAddr"))
            : nullptr;
        get_memory_fd = get_device_proc_addr
            ? reinterpret_cast<vk_get_memory_fd_fn>(
                    get_device_proc_addr(vk_device, "vkGetMemoryFdKHR"))
            : nullptr;
        get_memory_fd_properties = get_device_proc_addr
            ? reinterpret_cast<vk_get_memory_fd_properties_fn>(
                    get_device_proc_addr(vk_device, "vkGetMemoryFdPropertiesKHR"))
            : nullptr;
        int fd = -1;
        vk_memory_get_fd_info fd_info{1000074002U, nullptr,
                reinterpret_cast<void *>(static_cast<ULONG_PTR>(memory)), 1U};
        int vk_result = get_memory_fd
            ? get_memory_fd(vk_device, &fd_info, &fd)
            : -1000000;
        fprintf(stderr, "vkGetMemoryFdKHR result=%d fd=%d\n", vk_result, fd);
        fd_exported = vk_result == 0 && fd >= 0;
        if (fd_exported)
            exported_fd = fd;
        if (fd_exported && get_memory_fd_properties) {
            vk_memory_fd_properties properties{1000074001U, nullptr, 0U};
            int properties_result = get_memory_fd_properties(vk_device,
                    1U, fd, &properties);
            fprintf(stderr, "vkGetMemoryFdPropertiesKHR result=%d memory_type_bits=0x%x\n",
                    properties_result, properties.memoryTypeBits);
        }
    }
    fprintf(stderr, "vkd3d_memory_fd_exported=%s\n", fd_exported ? "yes" : "no");

    g_cuda_any_imported = false;
    /* The fd is intentionally consumed before releasing the D3D12 heap. */
    if (fd_exported) {
        HMODULE cuda = LoadLibraryA("nvcuda.dll");
        auto cuInit = cuda ? reinterpret_cast<cu_init_fn>(GetProcAddress(cuda, "cuInit")) : nullptr;
        auto cuDeviceGet = cuda ? reinterpret_cast<cu_device_get_fn>(GetProcAddress(cuda, "cuDeviceGet")) : nullptr;
        auto cuCtxCreate = cuda ? reinterpret_cast<cu_ctx_create_fn>(GetProcAddress(cuda, "cuCtxCreate_v2")) : nullptr;
        auto cuCtxDestroy = cuda ? reinterpret_cast<cu_ctx_destroy_fn>(GetProcAddress(cuda, "cuCtxDestroy_v2")) : nullptr;
        auto cuImportExternalMemory = cuda ? reinterpret_cast<cu_import_external_memory_fn>(GetProcAddress(cuda, "cuImportExternalMemory")) : nullptr;
        auto cuMapBuffer = cuda ? reinterpret_cast<cu_external_memory_get_mapped_buffer_fn>(GetProcAddress(cuda, "cuExternalMemoryGetMappedBuffer")) : nullptr;
        auto cuMemFree = cuda ? reinterpret_cast<cu_mem_free_fn>(GetProcAddress(cuda, "cuMemFree_v2")) : nullptr;
        auto cuDestroyExternalMemory = cuda ? reinterpret_cast<cu_destroy_external_memory_fn>(GetProcAddress(cuda, "cuDestroyExternalMemory")) : nullptr;

        fprintf(stderr, "nvcuda module=%p symbols: init=%p device_get=%p ctx_create=%p ctx_destroy=%p import=%p map=%p mem_free=%p destroy=%p\n",
                cuda, cuInit, cuDeviceGet, cuCtxCreate, cuCtxDestroy,
                cuImportExternalMemory, cuMapBuffer, cuMemFree,
                cuDestroyExternalMemory);

        cuda_import_result cuda_result{};
        CUresult init_rc = (cuInit && cuDeviceGet && cuCtxCreate && cuCtxDestroy &&
                cuImportExternalMemory && cuMapBuffer && cuMemFree &&
                cuDestroyExternalMemory) ? cuInit(0) : -1;
        fprintf(stderr, "CUDA external-memory symbols/init result=%d\n", init_rc);
        cuda_result.initialized = init_rc == 0;
        if (cuda_result.initialized) {
            /* This probe creates the heap on GPU A.  Importing that same
             * allocation as if it came from GPU B would be a false negative;
             * use the UUID-matched CUDA source and the other GPU as destination. */
            const int source_ordinal = 0;
            const int destination_ordinal = 1;
            for (int ordinal = source_ordinal; ordinal <= source_ordinal; ++ordinal) {
                CUdevice cuda_device = -1;
                CUcontext context = nullptr;
                CUexternalMemory external_memory = nullptr;
                CUdeviceptr mapped = 0;
                int import_fd = -1;
                vk_memory_get_fd_info import_fd_info{1000074002U, nullptr,
                        reinterpret_cast<void *>(static_cast<ULONG_PTR>(memory)), 1U};
                int export_rc = get_memory_fd
                    ? get_memory_fd(vk_device, &import_fd_info, &import_fd)
                    : -1000000;
                CUresult device_rc = cuDeviceGet(&cuda_device, ordinal);
                CUresult ctx_rc = device_rc == 0 ? cuCtxCreate(&context, 0, cuda_device) : device_rc;

                /* CUDA takes ownership of an opaque-fd import on success. */
                cuda_external_memory_handle_desc import_desc{};
                import_desc.type = 1; /* CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD */
                import_desc.handle.fd = import_fd;
                import_desc.size = heap_desc.SizeInBytes;
                cuda_external_memory_buffer_desc buffer_desc{};
                buffer_desc.offset = 0;
                buffer_desc.size = heap_desc.SizeInBytes;
                CUresult import_rc = export_rc == 0 && ctx_rc == 0
                    ? cuImportExternalMemory(&external_memory, &import_desc)
                    : (export_rc != 0 ? static_cast<CUresult>(export_rc) : ctx_rc);
                CUresult map_rc = import_rc == 0
                    ? cuMapBuffer(&mapped, external_memory, &buffer_desc)
                    : import_rc;
                fprintf(stderr, "CUDA import GPU%d: fd=%d export_rc=%d device=%d device_rc=%d ctx_rc=%d import_rc=%d map_rc=%d mapped=0x%llx\n",
                        ordinal, import_fd, export_rc, cuda_device, device_rc, ctx_rc, import_rc, map_rc,
                        (unsigned long long)mapped);
                bool imported = import_rc == 0 && map_rc == 0 && mapped != 0;
                g_cuda_any_imported = g_cuda_any_imported || imported;
                if (ordinal == 0) cuda_result.gpu0_imported = imported;
                else cuda_result.gpu1_imported = imported;
                if (mapped && cuMemFree) cuMemFree(mapped);
                if (external_memory && cuDestroyExternalMemory) cuDestroyExternalMemory(external_memory);
                if (context && cuCtxDestroy) cuCtxDestroy(context);
            }
        }
        fprintf(stderr, "cuda_import_gpu0=%s cuda_import_gpu1=%s\n",
                cuda_result.gpu0_imported ? "yes" : "no",
                cuda_result.gpu1_imported ? "yes" : "no");
    }

    /* A native Linux helper can use the Unix-side CUDA implementation that
     * Proton intentionally does not expose as Windows PE exports. */
    const char *helper_path = getenv("MGPU_CUDA_IMPORT_HELPER");
    if (fd_exported && helper_path && *helper_path && get_memory_fd) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        auto spawn_unix = ntdll
            ? reinterpret_cast<wine_unix_spawnvp_fn>(
                    GetProcAddress(ntdll, "__wine_unix_spawnvp"))
            : nullptr;
        fprintf(stderr, "wine unix helper=%s spawn=%p\n", helper_path, spawn_unix);
        if (spawn_unix) {
            char size_text[32];
            snprintf(size_text, sizeof(size_text), "%llu",
                    (unsigned long long)heap_desc.SizeInBytes);
            /* The allocation was created on GPU A. Importing its FD as if it
             * originated on GPU B would be a false negative. */
            const int source_ordinal = 0;
            const int destination_ordinal = 1;
            for (int ordinal = source_ordinal; ordinal <= source_ordinal; ++ordinal) {
                int helper_fd = -1;
                vk_memory_get_fd_info helper_fd_info{1000074002U, nullptr,
                        reinterpret_cast<void *>(static_cast<ULONG_PTR>(memory)), 1U};
                int export_rc = get_memory_fd(vk_device, &helper_fd_info, &helper_fd);
                char fd_text[32];
                char ordinal_text[16];
                snprintf(fd_text, sizeof(fd_text), "%d", helper_fd);
                snprintf(ordinal_text, sizeof(ordinal_text), "%d", ordinal);
                char destination_text[16];
                snprintf(destination_text, sizeof(destination_text), "%d", destination_ordinal);
                char *argv[] = {const_cast<char *>(helper_path), fd_text,
                        size_text, ordinal_text, destination_text, nullptr};
                if (export_rc == 0)
                    SetEnvironmentVariableA("MGPU_INHERIT_FD", fd_text);
                LONG spawn_rc = export_rc == 0
                    ? spawn_unix(argv, 1)
                    : static_cast<LONG>(export_rc);
                if (export_rc == 0)
                    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
                fprintf(stderr, "CUDA helper GPU%d: fd=%d export_rc=%d spawn_rc=%ld\n",
                        ordinal, helper_fd, export_rc, (long)spawn_rc);
                if (spawn_rc == 0)
                    g_cuda_helper_spawned = true;
            }
        }
    }

    interop->lpVtbl->Release(interop);
    heap->Release();
    return SUCCEEDED(hr) && memory != 0;
}

static bool inspect_base_interop(ID3D12Device *device)
{
    vkd3d_interop_device *interop = nullptr;
    HRESULT hr = device->QueryInterface(IID_ID3D12DXVKInteropDevice, (void **)&interop);
    log_hr("QueryInterface ID3D12DXVKInteropDevice", hr);
    if (FAILED(hr) || !interop)
        return false;

    VkInstance instance = nullptr;
    VkPhysicalDevice physical = nullptr;
    VkDevice vk_device = nullptr;
    hr = interop->lpVtbl->GetVulkanHandles(interop, &instance, &physical, &vk_device);
    log_hr("DXVK interop GetVulkanHandles", hr);
    fprintf(stderr, "DXVK interop handles: instance=%p physical=%p device=%p\n",
            instance, physical, vk_device);

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource_desc{};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = 4096;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&resource));
    log_hr("CreateCommittedResource for VKD3D interop", hr);
    UINT64 vk_handle = 0;
    UINT64 buffer_offset = 0;
    if (SUCCEEDED(hr))
    {
        hr = interop->lpVtbl->GetVulkanResourceInfo(interop, resource,
                &vk_handle, &buffer_offset);
        log_hr("GetVulkanResourceInfo", hr);
        fprintf(stderr, "VKD3D resource: handle=0x%llx offset=%llu\n",
                (unsigned long long)vk_handle,
                (unsigned long long)buffer_offset);
        resource->Release();
    }
    interop->lpVtbl->Release(interop);
    return SUCCEEDED(hr) && instance && physical && vk_device && vk_handle;
}

int main()
{
    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    log_hr("CreateDXGIFactory1", hr);
    if (FAILED(hr)) return 2;

    UINT cursor = 0;
    IDXGIAdapter1 *adapter_a = find_3090(factory, &cursor);
    IDXGIAdapter1 *adapter_b = find_3090(factory, &cursor);
    if (!adapter_a || !adapter_b)
    {
        fprintf(stderr, "No se encontraron dos RTX 3090 en DXGI.\n");
        if (adapter_b) adapter_b->Release();
        if (adapter_a) adapter_a->Release();
        factory->Release();
        return 3;
    }

    ID3D12Device *device_a = nullptr;
    ID3D12Device *device_b = nullptr;
    hr = D3D12CreateDevice(adapter_a, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_a));
    log_hr("D3D12CreateDevice A", hr);
    if (FAILED(hr)) return 4;
    hr = D3D12CreateDevice(adapter_b, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_b));
    log_hr("D3D12CreateDevice B", hr);
    if (FAILED(hr)) return 5;

    device_handles handles_a = inspect_device("GPU A", device_a);
    device_handles handles_b = inspect_device("GPU B", device_b);
    bool base_interop = inspect_base_interop(device_a);
    fprintf(stderr, "vkd3d_base_interop=%s\n", base_interop ? "yes" : "no");
    bool heap_interop = inspect_heap_interop(device_a);
    fprintf(stderr, "vkd3d_heap_memory_exported=%s\n", heap_interop ? "yes" : "no");
    bool distinct = handles_a.valid && handles_b.valid &&
        handles_a.physical != handles_b.physical && handles_a.device != handles_b.device;
    fprintf(stderr, "multi_adapter_distinct=%s\n", distinct ? "yes" : "no");
    fprintf(stderr, "Nota: VKD3D_VULKAN_DEVICE selecciona un device Vulkan por proceso;\n"
                    "crear dos ID3D12Device no garantiza dos adapters distintos.\n");

    device_b->Release();
    device_a->Release();
    adapter_b->Release();
    adapter_a->Release();
    factory->Release();
    if (!handles_a.valid || !handles_b.valid)
        return 6;
    if (getenv("VKD3D_INTEROP_REQUIRE_HEAP") &&
        *getenv("VKD3D_INTEROP_REQUIRE_HEAP") && !heap_interop)
        return 8;
    if (getenv("VKD3D_INTEROP_REQUIRE_CUDA_IMPORT") &&
        *getenv("VKD3D_INTEROP_REQUIRE_CUDA_IMPORT") && !g_cuda_any_imported)
        return 9;
    if (getenv("VKD3D_INTEROP_REQUIRE_CUDA_HELPER") &&
        *getenv("VKD3D_INTEROP_REQUIRE_CUDA_HELPER") && !g_cuda_helper_spawned)
        return 10;
    if (getenv("VKD3D_INTEROP_REQUIRE_DISTINCT") &&
        *getenv("VKD3D_INTEROP_REQUIRE_DISTINCT") && !distinct)
        return 7;
    return 0;
}
