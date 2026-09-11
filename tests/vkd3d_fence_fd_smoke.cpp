#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <vulkan.h>
#include <cstdio>

struct vkd3d_interop_device;
struct vkd3d_interop_device5_vtbl {
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
    HRESULT (STDMETHODCALLTYPE *ExportVulkanHeapFd)(vkd3d_interop_device *, ID3D12Heap *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanFenceFd)(vkd3d_interop_device *, ID3D12Fence *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanPhysicalDeviceIdentity)(vkd3d_interop_device *, UINT8 *, UINT32 *, UINT32 *, UINT32 *, UINT32 *);
};
struct vkd3d_interop_device { const vkd3d_interop_device5_vtbl *lpVtbl; };

static const GUID IID_ID3D12DXVKInteropDevice5 =
    {0x5f7f64b7, 0x8e0d, 0x4aa8, {0x9e, 0x29, 0x4b, 0x2f, 0x1b, 0x3d, 0x7e, 0x61}};

static void log_hr(const char *label, HRESULT hr)
{
    std::printf("%s=0x%08lx\n", label, (unsigned long)hr);
}

int main()
{
    ID3D12Device *d3d12 = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&d3d12));
    log_hr("create_device", hr);
    if (FAILED(hr))
        return 2;

    ID3D12Fence *fence = nullptr;
    hr = d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&fence));
    log_hr("create_shared_fence", hr);
    if (FAILED(hr)) {
        d3d12->Release();
        return 3;
    }

    vkd3d_interop_device *interop = nullptr;
    hr = d3d12->QueryInterface(IID_ID3D12DXVKInteropDevice5,
            reinterpret_cast<void **>(&interop));
    log_hr("query_interop_device5", hr);
    if (FAILED(hr) || !interop) {
        fence->Release();
        d3d12->Release();
        return 4;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    hr = interop->lpVtbl->GetVulkanHandles(interop, &instance, &physical, &device);
    log_hr("get_vulkan_handles", hr);
    if (FAILED(hr) || !instance || !physical || !device) {
        interop->lpVtbl->Release(interop);
        fence->Release();
        d3d12->Release();
        return 5;
    }

    INT fd = -1;
    hr = interop->lpVtbl->ExportVulkanFenceFd(interop, fence,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &fd);
    log_hr("export_fence_fd", hr);
    std::printf("exported_fd=%d\n", fd);
    if (FAILED(hr) || fd < 0) {
        interop->lpVtbl->Release(interop);
        fence->Release();
        d3d12->Release();
        return 6;
    }

    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    auto get_instance = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    auto get_device = get_instance ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            get_instance(instance, "vkGetDeviceProcAddr")) : nullptr;
    auto create_semaphore = get_device ? reinterpret_cast<PFN_vkCreateSemaphore>(
            get_device(device, "vkCreateSemaphore")) : nullptr;
    auto import_fd = get_device ? reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            get_device(device, "vkImportSemaphoreFdKHR")) : nullptr;
    auto get_counter = get_device ? reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
            get_device(device, "vkGetSemaphoreCounterValue")) : nullptr;
    auto wait_semaphores = get_device ? reinterpret_cast<PFN_vkWaitSemaphores>(
            get_device(device, "vkWaitSemaphores")) : nullptr;
    auto destroy_semaphore = get_device ? reinterpret_cast<PFN_vkDestroySemaphore>(
            get_device(device, "vkDestroySemaphore")) : nullptr;
    std::printf("vulkan_import_proc=%s counter_proc=%s wait_proc=%s\n",
            import_fd ? "yes" : "no", get_counter ? "yes" : "no",
            wait_semaphores ? "yes" : "no");
    if (!create_semaphore || !import_fd || !get_counter || !wait_semaphores || !destroy_semaphore) {
        interop->lpVtbl->Release(interop);
        fence->Release();
        d3d12->Release();
        return 7;
    }

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &type_info;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = create_semaphore(device, &create_info, nullptr, &semaphore);
    std::printf("create_import_semaphore=%d\n", (int)result);
    if (result != VK_SUCCESS) {
        interop->lpVtbl->Release(interop);
        fence->Release();
        d3d12->Release();
        return 8;
    }

    VkImportSemaphoreFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = semaphore;
    import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = fd;
    result = import_fd(device, &import_info);
    std::printf("import_fence_fd=%d\n", (int)result);
    if (result != VK_SUCCESS) {
        destroy_semaphore(device, semaphore, nullptr);
        interop->lpVtbl->Release(interop);
        fence->Release();
        d3d12->Release();
        return 9;
    }
    fd = -1;

    uint64_t before = UINT64_MAX;
    result = get_counter(device, semaphore, &before);
    std::printf("counter_before_signal_result=%d value=%llu\n", (int)result,
            (unsigned long long)before);
    hr = fence->Signal(1);
    log_hr("d3d12_signal_1", hr);

    uint64_t after = 0;
    result = get_counter(device, semaphore, &after);
    std::printf("counter_after_signal_result=%d value=%llu\n", (int)result,
            (unsigned long long)after);
    VkSemaphoreWaitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &semaphore;
    uint64_t value = 1;
    wait_info.pValues = &value;
    result = wait_semaphores(device, &wait_info, 1000000000ull);
    std::printf("vulkan_wait_value_1=%d\n", (int)result);

    bool passed = SUCCEEDED(hr) && result == VK_SUCCESS && before == 0 && after >= 1;
    std::printf("d3d12_fence_fd_roundtrip=%s\n", passed ? "pass" : "fail");
    destroy_semaphore(device, semaphore, nullptr);
    interop->lpVtbl->Release(interop);
    fence->Release();
    d3d12->Release();
    return passed ? 0 : 10;
}
